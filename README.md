# 750RTX-terminal

Text-mode control panels for the [750RTX](https://github.com/alberto-grl/750RTX)
STM32H750 SDR transceiver, to run on a Linux PC connected to the radio's
USB serial port (or its UART console).

The radio accepts single-character commands and streams its status as an
ANSI screen. Both clients emulate that screen, parse the status fields, and
offer a button for every command. Each button shows its keyboard shortcut,
and buttons can also be clicked with the mouse.

Two implementations are provided:

| File | Language | Use it on |
|---|---|---|
| `rtx_term.c` | C99 + ncurses | Any Linux, including old/32-bit systems (e.g. Ubuntu 18.04 i386) |
| `rtx_client.py` | Python 3.8+ + [Textual](https://textual.textualize.io/) | Recent Linux distributions |

## C / ncurses version

```bash
sudo apt install build-essential libncurses5-dev
gcc -std=c99 -O2 -Wall -o rtx_term rtx_term.c -lncurses
./rtx_term /dev/ttyACM0
```

## Python / Textual version

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python rtx_client.py /dev/ttyACM0
```

The baud rate can be set with `-b 115200` (C) or `--baud 115200` (Python);
it only matters for the UART console, not for USB CDC.

Quit with **Ctrl+Q** (also Ctrl+C or F10 in the C version).

## Screen

- **Status**: frequency (kHz), tuning step, mode, bandwidth, AGC, TX power,
  keyer WPM, volume, TX frequency error, S-meter bar and an OVF (ADC overload)
  indicator.
- **Radio screen**: the radio's own output, including the colored waterfall
  and the CW decoder / SCAMP text when the firmware is built with them.
- **Commands**: every command as a button; the current mode, bandwidth, AGC,
  step and power are highlighted.

## Commands

| Key | Action | Key | Action |
|---|---|---|---|
| `1` / `2` | Frequency down / up by half a step | `t` / `r` | TX on / back to RX |
| `3` | Step 1 Hz | `J` / `K` / `L` | TX power low / mid / max |
| `4` | Step 10 Hz | `C` / `D` | Send CW message 0 / 1 |
| `5` | Step 100 Hz | `g` / `h` | Keyer WPM down / up |
| `6` | Step 1 kHz | `F` / `G` | Digital modes on / off |
| `7` | Step 10 kHz | `Y` | Send WSPR |
| `8` | Step 100 kHz | `z` | Send SCAMP (needs `USE_SCAMP`) |
| `9` | Step 9 kHz (MW) | `b` / `B` | Radio display on / off |
| `l` `u` `a` `c` | LSB / USB / AM / CW | `W` | Waterfall on/off (needs `TEST_WF`) |
| `n` / `w` | Narrow / wide filter | `Z` | Reset clock |
| `f` / `s` | AGC fast / slow | `v` | PLL test |
| `-` / `+` | Volume down / up | | |

**Note:** a single `t` keystroke switches the transmitter on.

## Notes

- On connect the client sends `b` (display on), so the radio redraws its
  status immediately.
- Commands are sent one byte at a time, at most one every 30 ms: the firmware
  only acts on 1-byte USB packets and handles one per main-loop pass.
- If the radio resets or the cable is unplugged, the client keeps retrying the
  port and reconnects automatically.
- Use a 256-color terminal (`TERM=xterm-256color`) for the waterfall. If box
  borders appear as letters such as `lqqk` (e.g. PuTTY with UTF-8), run the C
  version with `NCURSES_NO_UTF8_ACS=1`.
- Your user must be in the `dialout` group to open `/dev/ttyACM*`. Prefer the
  stable `/dev/serial/by-id/...STM32_Virtual_ComPort...` link over `ttyACMn`.

## License

Copyright (C) 2026 Alberto Garlassi

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).
