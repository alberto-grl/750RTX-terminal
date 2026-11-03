#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Text-mode control panel for the 750RTX SDR transceiver.

The radio's USB CDC (or UART) console accepts single-character commands and
streams its status as an ANSI screen (cursor positioning + 256-color SGR).
This client emulates that screen, parses the status fields, and offers a
button for every command; each button shows its keyboard shortcut.

Usage:
    python3 rtx_client.py /dev/ttyACM0 [--baud 115200]

Requires: pip install textual pyserial
"""

import argparse
import re
import threading
import time
import queue

import serial
from rich.text import Text
from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.css.query import NoMatches
from textual.widgets import Button, Footer, Header, Label, Static

# --------------------------------------------------------------------------
# Command table: (key, label, group, variant)
# Keys and actions mirror the switch in Core/Src/main.c.
# --------------------------------------------------------------------------
COMMANDS = [
    ("1", "Freq -", "Tuning", "primary"),
    ("2", "Freq +", "Tuning", "primary"),
    ("3", "1 Hz", "Step", "default"),       # SetFstep(5): 10^(5-idx) Hz
    ("4", "10 Hz", "Step", "default"),
    ("5", "100 Hz", "Step", "default"),
    ("6", "1 kHz", "Step", "default"),
    ("7", "10 kHz", "Step", "default"),
    ("8", "100 kHz", "Step", "default"),
    ("9", "9 kHz MW", "Step", "default"),
    ("l", "LSB", "Mode", "default"),
    ("u", "USB", "Mode", "default"),
    ("a", "AM", "Mode", "default"),
    ("c", "CW", "Mode", "default"),
    ("n", "Narrow", "Filter", "default"),
    ("w", "Wide", "Filter", "default"),
    ("f", "AGC fast", "Filter", "default"),
    ("s", "AGC slow", "Filter", "default"),
    ("-", "Vol -", "Audio", "default"),
    ("+", "Vol +", "Audio", "default"),
    ("t", "TX ON", "Transmit", "error"),
    ("r", "RX (TX off)", "Transmit", "success"),
    ("J", "Pwr low", "Transmit", "default"),
    ("K", "Pwr mid", "Transmit", "default"),
    ("L", "Pwr max", "Transmit", "default"),
    ("C", "CW msg 0", "CW keyer", "warning"),
    ("D", "CW msg 1", "CW keyer", "warning"),
    ("g", "WPM -", "CW keyer", "default"),
    ("h", "WPM +", "CW keyer", "default"),
    ("F", "Digital ON", "Digital", "default"),
    ("G", "Digital OFF", "Digital", "default"),
    ("Y", "Send WSPR", "Digital", "error"),
    ("z", "Send SCAMP", "Digital", "error"),
    ("b", "Display ON", "Display", "default"),
    ("B", "Display OFF", "Display", "default"),
    ("W", "Waterfall", "Display", "default"),
    ("Z", "Clock reset", "Misc", "default"),
    ("v", "PLL test", "Misc", "default"),
]
KEY_TO_LABEL = {k: lbl for k, lbl, _, _ in COMMANDS}

# Which button lights up for a given parsed status value
STEP_TO_KEY = {"1": "3", "10": "4", "100": "5", "1K": "6", "10K": "7", "100K": "8", "9K": "9"}
MODE_TO_KEY = {"LSB": "l", "USB": "u", "AM": "a", "CW": "c"}
BW_TO_KEY = {"Narrow": "n", "Wide": "w"}
AGC_TO_KEY = {"Fast": "f", "Slow": "s"}
PWR_TO_KEY = {"Low": "J", "Mid": "K", "Max": "L"}

ROWS, COLS = 24, 80


def button_id(key: str) -> str:
    return f"cmd-{ord(key)}"


# --------------------------------------------------------------------------
# Minimal ANSI screen emulator (only what the firmware emits)
# --------------------------------------------------------------------------
class Screen:
    CSI_RE = re.compile(r"\x1b\[([0-9;]*)([A-Za-z])")

    def __init__(self, rows=ROWS, cols=COLS):
        self.rows, self.cols = rows, cols
        self.lock = threading.Lock()
        self.clear()
        self.pending = ""

    def clear(self):
        self.chars = [[" "] * self.cols for _ in range(self.rows)]
        self.bg = [[None] * self.cols for _ in range(self.rows)]
        self.fg = [[None] * self.cols for _ in range(self.rows)]
        self.r = self.c = 0
        self.cur_fg = self.cur_bg = None

    def feed(self, data: str):
        with self.lock:
            data = self.pending + data
            self.pending = ""
            i, n = 0, len(data)
            while i < n:
                ch = data[i]
                if ch == "\x1b":
                    m = self.CSI_RE.match(data, i)
                    if m:
                        self._csi(m.group(1), m.group(2))
                        i = m.end()
                        continue
                    if n - i < 16:  # possibly truncated sequence: wait for more
                        self.pending = data[i:]
                        break
                    i += 1  # unknown escape, skip ESC
                    continue
                if ch == "\r":
                    self.c = 0
                elif ch == "\n":
                    self.r = min(self.r + 1, self.rows - 1)
                elif ch == "\b":
                    self.c = max(self.c - 1, 0)
                elif ch >= " ":
                    if self.c < self.cols:
                        self.chars[self.r][self.c] = ch
                        self.bg[self.r][self.c] = self.cur_bg
                        self.fg[self.r][self.c] = self.cur_fg
                    self.c += 1
                i += 1

    def _csi(self, params: str, cmd: str):
        p = [int(x) if x else 0 for x in params.split(";")] if params else []
        if cmd in "Hf":
            row = (p[0] if len(p) > 0 else 1) or 1
            col = (p[1] if len(p) > 1 else 1) or 1
            self.r = min(row, self.rows) - 1
            self.c = min(col, self.cols) - 1
        elif cmd == "m":
            self._sgr(p or [0])
        elif cmd == "K":
            for c in range(self.c, self.cols):
                self.chars[self.r][c] = " "
                self.bg[self.r][c] = self.fg[self.r][c] = None
        elif cmd == "J" and (p[:1] == [2]):
            r, c = self.r, self.c
            self.clear()
            self.r, self.c = r, c
        elif cmd == "A":
            self.r = max(self.r - (p[0] if p else 1), 0)
        elif cmd == "B":
            self.r = min(self.r + (p[0] if p else 1), self.rows - 1)
        elif cmd == "C":
            self.c = min(self.c + (p[0] if p else 1), self.cols - 1)
        elif cmd == "D":
            self.c = max(self.c - (p[0] if p else 1), 0)
        # anything else (e.g. ESC[T scroll) is ignored

    def _sgr(self, p):
        i = 0
        while i < len(p):
            v = p[i]
            if v == 0:
                self.cur_fg = self.cur_bg = None
            elif v in (38, 48) and i + 2 < len(p) and p[i + 1] == 5:
                if v == 38:
                    self.cur_fg = p[i + 2]
                else:
                    # firmware resets the waterfall with background 16 (black)
                    self.cur_bg = None if p[i + 2] == 16 else p[i + 2]
                i += 2
            elif 30 <= v <= 37:
                self.cur_fg = v - 30
            elif 40 <= v <= 47:
                self.cur_bg = v - 40
            elif v == 39:
                self.cur_fg = None
            elif v == 49:
                self.cur_bg = None
            i += 1

    def line(self, r: int) -> str:
        with self.lock:
            return "".join(self.chars[r])

    def render(self) -> Text:
        out = Text()
        with self.lock:
            last = self.rows
            while last > 0 and not "".join(self.chars[last - 1]).strip() \
                    and not any(self.bg[last - 1]):
                last -= 1
            for r in range(max(last, 1)):
                if r:
                    out.append("\n")
                for c in range(self.cols):
                    style = ""
                    if self.fg[r][c] is not None:
                        style += f"color({self.fg[r][c]})"
                    if self.bg[r][c] is not None:
                        style += f" on color({self.bg[r][c]})"
                    out.append(self.chars[r][c], style=style.strip() or None)
        # no rstrip(): the waterfall is made of colored spaces
        return out


# --------------------------------------------------------------------------
# Status parser (rows/format from main.c DisplayStatus and the S-meter line)
# --------------------------------------------------------------------------
FREQ_RE = re.compile(r"Freq\s+(-?[\d.]+)\s+Step\s+(\S+)")
STATUS_RE = re.compile(
    r"Mode\s+(\S+)\s+BW\s+(\S+)\s+AGG\s+(\S+)\s+ERR\s+(-?\d+)\s+WPM\s+(\d+)\s+"
    r"PWR\s+(\S+)\s+Volume\s+([\d.]+)")
SMETER_RE = re.compile(r"^S\s+(-?[\d.]+|-?inf|nan)")
TIME_RE = re.compile(r"T\s+(\d+):\s*(\d+):\s*(\d+)")


def parse_status(screen: Screen) -> dict:
    st = {}
    l1, l3, l4, l5 = (screen.line(i) for i in (0, 2, 3, 4))
    if m := SMETER_RE.match(l1):
        st["s"] = m.group(1)
    if m := TIME_RE.search(l1):
        st["time"] = "%s:%02d:%02d" % (m.group(1), int(m.group(2)), int(m.group(3)))
    if m := FREQ_RE.search(l3):
        st["freq"], st["step"] = m.group(1), m.group(2)
    if m := STATUS_RE.search(l5):
        (st["mode"], st["bw"], st["agc"], st["err"],
         st["wpm"], st["pwr"], st["vol"]) = m.groups()
    st["ovf"] = "OVF" in l4
    return st


# --------------------------------------------------------------------------
# Serial link: reader thread + paced writer (one byte per USB packet)
# --------------------------------------------------------------------------
class RadioLink:
    SEND_GAP = 0.03  # s; firmware only handles one 1-byte packet per loop

    def __init__(self, port, baud, on_data, on_state):
        self.port, self.baud = port, baud
        self.on_data, self.on_state = on_data, on_state
        self.ser = None
        self.txq = queue.Queue()
        self.stop = threading.Event()
        self.connected = False

    def start(self):
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._writer, daemon=True).start()

    def send(self, key: str):
        self.txq.put(key.encode("ascii"))

    def close(self):
        self.stop.set()
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass

    def _set_state(self, ok, msg):
        self.connected = ok
        self.on_state(ok, msg)

    def _reader(self):
        while not self.stop.is_set():
            if self.ser is None:
                try:
                    self.ser = serial.Serial(self.port, self.baud, timeout=0.1)
                    self._set_state(True, f"Connected to {self.port}")
                    self.send("b")  # display on: also makes the radio redraw its status
                except (serial.SerialException, OSError) as e:
                    self._set_state(False, f"{self.port}: {e}. Retrying...")
                    self.stop.wait(2)
                    continue
            try:
                data = self.ser.read(4096)
                if data:
                    self.on_data(data.decode("latin-1"))
            except (serial.SerialException, OSError, TypeError) as e:
                self._set_state(False, f"Lost {self.port}: {e}. Reconnecting...")
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                self.stop.wait(1)

    def _writer(self):
        while not self.stop.is_set():
            try:
                b = self.txq.get(timeout=0.2)
            except queue.Empty:
                continue
            ser = self.ser
            if ser is None:
                continue
            try:
                ser.write(b)
                ser.flush()
            except (serial.SerialException, OSError):
                pass
            time.sleep(self.SEND_GAP)


# --------------------------------------------------------------------------
# UI
# --------------------------------------------------------------------------
class CmdButton(Button, can_focus=False):
    """Non-focusable so every keystroke reaches the app's shortcut handler."""


class RTXClient(App):
    TITLE = "750RTX"
    CSS = """
    Screen { layout: vertical; }
    #top { height: auto; }
    #freqbox { width: 30; height: 5; border: round $accent; content-align: center middle; }
    #freq, #unit { width: 100%; content-align: center middle; }
    #freq { text-style: bold; color: $accent; }
    #unit { color: $text-muted; }
    #info { height: 5; border: round $primary; padding: 0 1; width: 1fr; }
    #term { border: round $panel-lighten-2; height: 1fr; min-height: 6; overflow-y: auto; }
    #buttons { height: auto; border: round $secondary; padding: 0 1; }
    .row { height: 1; }
    .grp { width: 10; color: $text-muted; }
    CmdButton { height: 1; min-width: 12; width: auto; border: none; margin: 0 1 0 0; padding: 0 1; }
    CmdButton.-active { text-style: bold reverse; }
    CmdButton.-flash { background: $warning; color: $text; }
    #msg { height: 1; padding: 0 1; color: $text-muted; }
    """
    BINDINGS = [("ctrl+q", "quit", "Quit")]

    def __init__(self, port, baud):
        super().__init__()
        self.screen_emu = Screen()
        self.dirty = threading.Event()
        self.link_msg = None
        self.link = RadioLink(port, baud, self._on_data, self._on_state)
        self.sub_title = port

    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal(id="top"):
            with Vertical(id="freqbox"):
                yield Static("---.---", id="freq")
                yield Static("kHz", id="unit")
            yield Static("", id="info")
        yield Static("", id="term")
        with Vertical(id="buttons"):
            groups = []
            for _, _, g, _ in COMMANDS:
                if g not in groups:
                    groups.append(g)
            for g in groups:
                with Horizontal(classes="row"):
                    yield Label(g, classes="grp")
                    for key, label, grp, variant in COMMANDS:
                        if grp == g:
                            yield CmdButton(Text(f"[{key}] {label}"), id=button_id(key),
                                            variant=variant)
        yield Static("Waiting for radio...", id="msg")
        yield Footer()

    def on_mount(self):
        self.query_one("#term").border_title = "Radio screen"
        self.query_one("#info").border_title = "Status"
        self.query_one("#buttons").border_title = "Commands  (key in brackets)"
        self.query_one("#freqbox").border_title = "Frequency"
        self.link.start()
        self.set_interval(0.1, self._refresh)

    def on_unmount(self):
        self.link.close()

    # --- link callbacks (called from worker threads) ---
    def _on_data(self, text):
        self.screen_emu.feed(text)
        self.dirty.set()

    def _on_state(self, ok, msg):
        # picked up by _refresh; never block the serial thread on the UI loop
        self.link_msg = (msg, not ok)
        self.dirty.set()

    # --- UI updates ---
    def _show_msg(self, msg, error=False):
        w = self.query_one("#msg", Static)
        w.update(Text(msg, style="bold red" if error else ""))

    def _refresh(self):
        if not self.dirty.is_set() or not self.is_running:
            return
        self.dirty.clear()
        try:
            self._update_widgets()
        except NoMatches:  # widgets gone while shutting down
            pass

    def _update_widgets(self):
        if self.link_msg:
            self._show_msg(*self.link_msg)
            self.link_msg = None
        self.query_one("#term", Static).update(self.screen_emu.render())
        st = parse_status(self.screen_emu)

        if "freq" in st:
            self.query_one("#freq", Static).update(st["freq"])

        info = Text()

        def field(name, value, style="bold"):
            info.append(f"{name} ", style="dim")
            info.append(f"{value}", style=style)
            info.append("   ")

        field("Mode", st.get("mode", "?"))
        field("BW", st.get("bw", "?"))
        field("AGC", st.get("agc", "?"))
        field("Step", (st.get("step", "?") + " Hz").replace("K Hz", " kHz"))
        info.append("\n")
        field("Power", st.get("pwr", "?"))
        field("WPM", st.get("wpm", "?"))
        field("Vol", st.get("vol", "?"))
        field("TX err", st.get("err", "?"))
        info.append("\n")
        s = st.get("s")
        field("S", s if s else "?")
        try:
            sval = max(0.0, min(float(s), 13.0)) if s else 0.0
        except ValueError:
            sval = 0.0
        bar = int(sval * 2)
        info.append("▮" * min(bar, 18), style="green")
        info.append("▮" * max(0, bar - 18), style="red")
        info.append("·" * max(0, 26 - bar), style="dim")
        info.append("   ")
        if st.get("time"):
            field("Time", st["time"])
        if st.get("ovf"):
            info.append(" OVF ", style="bold white on red")
        self.query_one("#info", Static).update(info)

        active = {
            STEP_TO_KEY.get(st.get("step", "").strip()),
            MODE_TO_KEY.get(st.get("mode")),
            BW_TO_KEY.get(st.get("bw")),
            AGC_TO_KEY.get(st.get("agc")),
            PWR_TO_KEY.get(st.get("pwr")),
        }
        for key, _, _, _ in COMMANDS:
            self.query_one(f"#{button_id(key)}").set_class(key in active, "-active")

    # --- input ---
    def _send(self, key):
        if not self.link.connected:
            self._show_msg("Not connected: command not sent", True)
            return
        self.link.send(key)
        self._show_msg(f"Sent '{key}': {KEY_TO_LABEL[key]}")

    def on_button_pressed(self, event: Button.Pressed):
        if event.button.id and event.button.id.startswith("cmd-"):
            self._send(chr(int(event.button.id[4:])))

    def on_key(self, event):
        ch = event.character
        if ch and ch in KEY_TO_LABEL:
            event.stop()
            self._send(ch)
            # flash the button as feedback (press() would post Pressed and resend)
            btn = self.query_one(f"#{button_id(ch)}", Button)
            btn.add_class("-flash")
            self.set_timer(0.15, lambda: btn.remove_class("-flash"))


def main():
    ap = argparse.ArgumentParser(description="Text-mode client for the 750RTX transceiver")
    ap.add_argument("tty", help="serial device, e.g. /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200,
                    help="baud rate (only matters for the UART console; default 115200)")
    args = ap.parse_args()
    RTXClient(args.tty, args.baud).run()


if __name__ == "__main__":
    main()
