/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rtx_term.c - text-mode control panel for the 750RTX SDR transceiver.
 *
 * The radio's USB CDC (or UART) console accepts single-character commands
 * and streams its status as an ANSI screen (cursor positioning + 256-color
 * SGR). This program emulates that screen, parses the status fields and
 * offers a button for every command; each button shows its shortcut key.
 * Buttons can be clicked with the mouse or triggered from the keyboard.
 *
 * Plain C99 + ncurses, so it builds on old/32-bit systems (Ubuntu 18.04 i386):
 *
 *     sudo apt install build-essential libncurses5-dev
 *     gcc -std=c99 -O2 -Wall -o rtx_term rtx_term.c -lncurses
 *
 * Usage:
 *     ./rtx_term /dev/ttyACM0 [-b 115200]
 *
 * Quit with Ctrl+Q, Ctrl+C or F10. Use a 256-color terminal
 * (TERM=xterm-256color) for a correctly colored waterfall.
 */

#define _DEFAULT_SOURCE
#include <ncurses.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <sys/select.h>

/* ------------------------------------------------------------------------
 * Command table (mirrors the switch in Core/Src/main.c)
 * ---------------------------------------------------------------------- */
enum { V_DEFAULT, V_PRIMARY, V_ERROR, V_SUCCESS, V_WARNING };

struct cmd {
	char key;
	const char *label;
	const char *group;
	int variant;
	int y, x, w;		/* screen position, filled by layout */
};

static struct cmd cmds[] = {
	{ '1', "Freq -",      "Tuning",   V_PRIMARY, 0, 0, 0 },
	{ '2', "Freq +",      "Tuning",   V_PRIMARY, 0, 0, 0 },
	{ '3', "1 Hz",        "Step",     V_DEFAULT, 0, 0, 0 },	/* SetFstep(5): 10^(5-idx) Hz */
	{ '4', "10 Hz",       "Step",     V_DEFAULT, 0, 0, 0 },
	{ '5', "100 Hz",      "Step",     V_DEFAULT, 0, 0, 0 },
	{ '6', "1 kHz",       "Step",     V_DEFAULT, 0, 0, 0 },
	{ '7', "10 kHz",      "Step",     V_DEFAULT, 0, 0, 0 },
	{ '8', "100 kHz",     "Step",     V_DEFAULT, 0, 0, 0 },
	{ '9', "9 kHz MW",    "Step",     V_DEFAULT, 0, 0, 0 },
	{ 'l', "LSB",         "Mode",     V_DEFAULT, 0, 0, 0 },
	{ 'u', "USB",         "Mode",     V_DEFAULT, 0, 0, 0 },
	{ 'a', "AM",          "Mode",     V_DEFAULT, 0, 0, 0 },
	{ 'c', "CW",          "Mode",     V_DEFAULT, 0, 0, 0 },
	{ 'n', "Narrow",      "Filter",   V_DEFAULT, 0, 0, 0 },
	{ 'w', "Wide",        "Filter",   V_DEFAULT, 0, 0, 0 },
	{ 'f', "AGC fast",    "Filter",   V_DEFAULT, 0, 0, 0 },
	{ 's', "AGC slow",    "Filter",   V_DEFAULT, 0, 0, 0 },
	{ '-', "Vol -",       "Audio",    V_DEFAULT, 0, 0, 0 },
	{ '+', "Vol +",       "Audio",    V_DEFAULT, 0, 0, 0 },
	{ 't', "TX ON",       "Transmit", V_ERROR, 0, 0, 0 },
	{ 'r', "RX (TX off)", "Transmit", V_SUCCESS, 0, 0, 0 },
	{ 'J', "Pwr low",     "Transmit", V_DEFAULT, 0, 0, 0 },
	{ 'K', "Pwr mid",     "Transmit", V_DEFAULT, 0, 0, 0 },
	{ 'L', "Pwr max",     "Transmit", V_DEFAULT, 0, 0, 0 },
	{ 'C', "CW msg 0",    "CW keyer", V_WARNING, 0, 0, 0 },
	{ 'D', "CW msg 1",    "CW keyer", V_WARNING, 0, 0, 0 },
	{ 'g', "WPM -",       "CW keyer", V_DEFAULT, 0, 0, 0 },
	{ 'h', "WPM +",       "CW keyer", V_DEFAULT, 0, 0, 0 },
	{ 'F', "Digital ON",  "Digital",  V_DEFAULT, 0, 0, 0 },
	{ 'G', "Digital OFF", "Digital",  V_DEFAULT, 0, 0, 0 },
	{ 'Y', "Send WSPR",   "Digital",  V_ERROR, 0, 0, 0 },
	{ 'z', "Send SCAMP",  "Digital",  V_ERROR, 0, 0, 0 },
	{ 'b', "Display ON",  "Display",  V_DEFAULT, 0, 0, 0 },
	{ 'B', "Display OFF", "Display",  V_DEFAULT, 0, 0, 0 },
	{ 'W', "Waterfall",   "Display",  V_DEFAULT, 0, 0, 0 },
	{ 'Z', "Clock reset", "Misc",     V_DEFAULT, 0, 0, 0 },
	{ 'v', "PLL test",    "Misc",     V_DEFAULT, 0, 0, 0 },
};
#define NCMDS ((int)(sizeof cmds / sizeof cmds[0]))

static const char *groups[] = {
	"Tuning", "Step", "Mode", "Filter", "Audio",
	"Transmit", "CW keyer", "Digital", "Display", "Misc"
};
#define NGROUPS ((int)(sizeof groups / sizeof groups[0]))

/* Parsed status value -> key of the button to highlight */
struct map { const char *val; char key; };
static const struct map step_map[] = {
	{ "1", '3' }, { "10", '4' }, { "100", '5' }, { "1K", '6' },
	{ "10K", '7' }, { "100K", '8' }, { "9K", '9' }, { NULL, 0 }
};
static const struct map mode_map[] = {
	{ "LSB", 'l' }, { "USB", 'u' }, { "AM", 'a' }, { "CW", 'c' }, { NULL, 0 }
};
static const struct map bw_map[]  = { { "Narrow", 'n' }, { "Wide", 'w' }, { NULL, 0 } };
static const struct map agc_map[] = { { "Fast", 'f' }, { "Slow", 's' }, { NULL, 0 } };
static const struct map pwr_map[] = {
	{ "Low", 'J' }, { "Mid", 'K' }, { "Max", 'L' }, { NULL, 0 }
};

static char map_lookup(const struct map *m, const char *v)
{
	for (; m->val; m++)
		if (strcmp(m->val, v) == 0)
			return m->key;
	return 0;
}

static struct cmd *find_cmd(int key)
{
	int i;
	for (i = 0; i < NCMDS; i++)
		if (cmds[i].key == key)
			return &cmds[i];
	return NULL;
}

/* ------------------------------------------------------------------------
 * Time helper
 * ---------------------------------------------------------------------- */
static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------------
 * Minimal ANSI screen emulator (only what the firmware emits)
 * ---------------------------------------------------------------------- */
#define EROWS 24
#define ECOLS 80

static char  ech[EROWS][ECOLS];
static short efg[EROWS][ECOLS], ebg[EROWS][ECOLS];
static int er, ec;			/* cursor */
static int cur_fg = -1, cur_bg = -1;	/* current SGR colors, -1 = default */
static int esc_state;			/* 0 text, 1 after ESC, 2 inside CSI */
static char csibuf[32];
static int csilen;

static void emu_clear(void)
{
	int r, c;
	for (r = 0; r < EROWS; r++)
		for (c = 0; c < ECOLS; c++) {
			ech[r][c] = ' ';
			efg[r][c] = ebg[r][c] = -1;
		}
	er = ec = 0;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void emu_sgr(const int *p, int np)
{
	int i;
	if (np == 0) {
		cur_fg = cur_bg = -1;
		return;
	}
	for (i = 0; i < np; i++) {
		int v = p[i];
		if (v == 0) {
			cur_fg = cur_bg = -1;
		} else if ((v == 38 || v == 48) && i + 2 < np && p[i + 1] == 5) {
			if (v == 38)
				cur_fg = p[i + 2];
			else	/* the firmware ends the waterfall with bg 16 (black) */
				cur_bg = p[i + 2] == 16 ? -1 : p[i + 2];
			i += 2;
		} else if (v >= 30 && v <= 37) {
			cur_fg = v - 30;
		} else if (v >= 40 && v <= 47) {
			cur_bg = v - 40;
		} else if (v == 39) {
			cur_fg = -1;
		} else if (v == 49) {
			cur_bg = -1;
		}
	}
}

static void emu_csi(char cmd)
{
	int p[16], np = 0, c;
	char *s = csibuf;

	if (*s) {
		for (;;) {
			if (np < 16)
				p[np++] = atoi(s);
			s = strchr(s, ';');
			if (!s)
				break;
			s++;
		}
	}
#define P(i, def) (np > (i) && p[i] ? p[i] : (def))
	switch (cmd) {
	case 'H':
	case 'f':
		er = clampi(P(0, 1), 1, EROWS) - 1;
		ec = clampi(P(1, 1), 1, ECOLS) - 1;	/* firmware sometimes uses column 0 */
		break;
	case 'm':
		emu_sgr(p, np);
		break;
	case 'K':
		for (c = ec; c < ECOLS; c++) {
			ech[er][c] = ' ';
			efg[er][c] = ebg[er][c] = -1;
		}
		break;
	case 'J':
		if (P(0, 0) == 2) {
			int r = er, cc = ec;
			emu_clear();
			er = r;
			ec = cc;
		}
		break;
	case 'A': er = clampi(er - P(0, 1), 0, EROWS - 1); break;
	case 'B': er = clampi(er + P(0, 1), 0, EROWS - 1); break;
	case 'C': ec = clampi(ec + P(0, 1), 0, ECOLS - 1); break;
	case 'D': ec = clampi(ec - P(0, 1), 0, ECOLS - 1); break;
	default: break;		/* e.g. ESC[T scroll: ignored */
	}
#undef P
}

static void emu_putc(unsigned char ch)
{
	switch (esc_state) {
	case 0:
		if (ch == 0x1b) {
			esc_state = 1;
		} else if (ch == '\r') {
			ec = 0;
		} else if (ch == '\n') {
			if (er < EROWS - 1)
				er++;
		} else if (ch == '\b') {
			if (ec > 0)
				ec--;
		} else if (ch >= 0x20) {
			if (ec < ECOLS) {
				ech[er][ec] = (ch < 0x7f) ? (char)ch : '?';
				efg[er][ec] = (short)cur_fg;
				ebg[er][ec] = (short)cur_bg;
			}
			ec++;
		}
		break;
	case 1:
		if (ch == '[') {
			esc_state = 2;
			csilen = 0;
		} else {
			esc_state = 0;
		}
		break;
	case 2:
		if (ch >= 0x30 && ch <= 0x3f) {		/* parameter bytes */
			if (csilen < (int)sizeof csibuf - 1)
				csibuf[csilen++] = (char)ch;
		} else if (ch >= 0x40 && ch <= 0x7e) {	/* final byte */
			csibuf[csilen] = 0;
			emu_csi((char)ch);
			esc_state = 0;
		} else if (ch < 0x20 || ch > 0x2f) {	/* not an intermediate byte */
			esc_state = 0;
		}
		break;
	}
}

/* ------------------------------------------------------------------------
 * Status parser (rows/format from main.c DisplayStatus and S-meter line)
 * ---------------------------------------------------------------------- */
struct status {
	int have_freq;
	double freq;
	char step[8];
	int have_st;
	char mode[8], bw[8], agc[8], pwr[8];
	int err, wpm;
	double vol;
	int have_s;
	double s;
	int have_time, hh, mm, ss;
	int ovf;
};

static void emu_row(int r, char *out)
{
	memcpy(out, ech[r], ECOLS);
	out[ECOLS] = 0;
}

static void parse_status(struct status *st)
{
	char l[ECOLS + 1];
	char *p;

	memset(st, 0, sizeof *st);

	emu_row(0, l);
	if (l[0] == 'S' && sscanf(l, "S %lf", &st->s) == 1)
		st->have_s = 1;
	if ((p = strstr(l, " T ")) != NULL &&
	    sscanf(p, " T %d:%d:%d", &st->hh, &st->mm, &st->ss) == 3)
		st->have_time = 1;

	emu_row(2, l);
	if ((p = strstr(l, "Freq")) != NULL &&
	    sscanf(p, "Freq %lf Step %7s", &st->freq, st->step) == 2)
		st->have_freq = 1;

	emu_row(3, l);
	st->ovf = strstr(l, "OVF") != NULL;

	emu_row(4, l);
	if ((p = strstr(l, "Mode")) != NULL &&
	    sscanf(p, "Mode %7s BW %7s AGG %7s ERR %d WPM %d PWR %7s Volume %lf",
		   st->mode, st->bw, st->agc, &st->err, &st->wpm, st->pwr,
		   &st->vol) == 7)
		st->have_st = 1;
}

/* ------------------------------------------------------------------------
 * Serial port
 * ---------------------------------------------------------------------- */
static const char *port_name;
static speed_t port_speed = B115200;
static int sfd = -1;

#define SEND_GAP_MS 30		/* firmware handles one 1-byte packet per loop */
static char txq[256];
static unsigned txhead, txtail;
static long long last_tx;

static char msg[200];
static int msg_is_error;

static void set_msg(int err, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void set_msg(int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	msg_is_error = err;
}

static speed_t baud_to_speed(long b)
{
	switch (b) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default: return 0;
	}
}

static int serial_open(void)
{
	struct termios t;
	int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);
		t.c_cflag |= CLOCAL | CREAD;
		cfsetispeed(&t, port_speed);
		cfsetospeed(&t, port_speed);
		tcsetattr(fd, TCSANOW, &t);
		tcflush(fd, TCIFLUSH);
	}
	return fd;
}

static void serial_close(void)
{
	if (sfd >= 0)
		close(sfd);
	sfd = -1;
	txhead = txtail = 0;
}

static void enqueue(char k)
{
	if (txtail - txhead < sizeof txq)
		txq[txtail++ % sizeof txq] = k;
}

static void pump_tx(long long now)
{
	if (sfd < 0 || txhead == txtail || now - last_tx < SEND_GAP_MS)
		return;
	if (write(sfd, &txq[txhead % sizeof txq], 1) == 1) {
		txhead++;
		last_tx = now;
	} else if (errno != EAGAIN && errno != EINTR) {
		set_msg(1, "Write error on %s: %s. Reconnecting...", port_name,
			strerror(errno));
		serial_close();
	}
}

/* ------------------------------------------------------------------------
 * Colors
 * ---------------------------------------------------------------------- */
enum {
	P_TITLE = 1, P_BTN_DEFAULT, P_BTN_PRIMARY, P_BTN_ERROR, P_BTN_SUCCESS,
	P_BTN_WARNING, P_FREQ, P_LABEL, P_BOX, P_ERRMSG, P_GOOD, P_BAD,
	P_FIRST_DYNAMIC = 16
};

static int use_color;
static short pair_cache[257][257];	/* [fg+1][bg+1] -> pair, 0 = not yet */
static int next_pair = P_FIRST_DYNAMIC;

/* Map an xterm-256 color to what the terminal has. */
static int map_color(int c)
{
	if (c < 0)
		return -1;
	if (COLORS >= 256)
		return c;
	if (c < 16)
		return c % 8;
	if (c < 232) {			/* 6x6x6 cube -> 8 basic colors */
		int v = c - 16, r = v / 36, g = (v / 6) % 6, b = v % 6;
		return (r >= 3 ? COLOR_RED : 0) | (g >= 3 ? COLOR_GREEN : 0) |
		       (b >= 2 ? COLOR_BLUE : 0);
	}
	return c < 244 ? COLOR_BLACK : COLOR_WHITE;
}

static attr_t emu_attr(int fg, int bg)
{
	short *slot;
	if (!use_color || (fg < 0 && bg < 0))
		return A_NORMAL;
	slot = &pair_cache[fg + 1][bg + 1];
	if (*slot == 0) {
		if (next_pair >= COLOR_PAIRS || next_pair > 32767)
			return A_NORMAL;
		init_pair((short)next_pair, (short)map_color(fg), (short)map_color(bg));
		*slot = (short)next_pair++;
	}
	return COLOR_PAIR(*slot);
}

static void init_colors(void)
{
	if (!has_colors())
		return;
	start_color();
	use_default_colors();
	use_color = 1;
	init_pair(P_TITLE, COLOR_BLACK, COLOR_CYAN);
	init_pair(P_BTN_DEFAULT, COLOR_BLACK, COLOR_WHITE);
	init_pair(P_BTN_PRIMARY, COLOR_WHITE, COLOR_BLUE);
	init_pair(P_BTN_ERROR, COLOR_WHITE, COLOR_RED);
	init_pair(P_BTN_SUCCESS, COLOR_BLACK, COLOR_GREEN);
	init_pair(P_BTN_WARNING, COLOR_BLACK, COLOR_YELLOW);
	init_pair(P_FREQ, COLOR_YELLOW, -1);
	init_pair(P_LABEL, COLOR_CYAN, -1);
	init_pair(P_BOX, COLOR_BLUE, -1);
	init_pair(P_ERRMSG, COLOR_RED, -1);
	init_pair(P_GOOD, COLOR_GREEN, -1);
	init_pair(P_BAD, COLOR_WHITE, COLOR_RED);
}

static attr_t cpair(int p)
{
	return use_color ? COLOR_PAIR(p) : A_NORMAL;
}

/* ------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------- */
static char flash_key;
static long long flash_until;

static void draw_box(int y, int x, int h, int w, const char *title)
{
	int i;
	if (h < 2 || w < 2)
		return;
	attron(cpair(P_BOX));
	mvaddch(y, x, ACS_ULCORNER);
	mvaddch(y, x + w - 1, ACS_URCORNER);
	mvaddch(y + h - 1, x, ACS_LLCORNER);
	mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
	mvhline(y, x + 1, ACS_HLINE, w - 2);
	mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
	for (i = 1; i < h - 1; i++) {
		mvaddch(y + i, x, ACS_VLINE);
		mvaddch(y + i, x + w - 1, ACS_VLINE);
	}
	attroff(cpair(P_BOX));
	if (title) {
		attron(cpair(P_LABEL) | A_BOLD);
		mvprintw(y, x + 2, " %s ", title);
		attroff(cpair(P_LABEL) | A_BOLD);
	}
}

static void field(const char *name, const char *value)
{
	attron(cpair(P_LABEL));
	printw("%s ", name);
	attroff(cpair(P_LABEL));
	attron(A_BOLD);
	printw("%-7s", value);
	attroff(A_BOLD);
	printw("  ");
}

#define GROUP_COL 12

/* Lay out buttons inside width w; returns number of lines used. */
static int layout_buttons(int top, int w)
{
	int g, i, line = 0;
	for (g = 0; g < NGROUPS; g++) {
		int x = GROUP_COL + 1;
		for (i = 0; i < NCMDS; i++) {
			struct cmd *c = &cmds[i];
			if (strcmp(c->group, groups[g]) != 0)
				continue;
			c->w = (int)strlen(c->label) + 6;	/* " [k] label " */
			if (x + c->w > w - 1 && x > GROUP_COL + 1) {
				x = GROUP_COL + 1;
				line++;
			}
			c->y = top + line;
			c->x = x;
			x += c->w + 1;
		}
		line++;
	}
	return line;
}

static void draw_buttons(int top, const char *active)
{
	int g, i, shown;
	long long now = now_ms();
	for (g = 0; g < NGROUPS; g++) {
		shown = 0;
		for (i = 0; i < NCMDS; i++) {
			struct cmd *c = &cmds[i];
			attr_t a;
			if (strcmp(c->group, groups[g]) != 0)
				continue;
			if (!shown) {
				attron(cpair(P_LABEL));
				mvprintw(c->y, 2, "%-*s", GROUP_COL - 2, groups[g]);
				attroff(cpair(P_LABEL));
				shown = 1;
			}
			if (c->x + c->w >= COLS)
				continue;
			a = cpair(P_BTN_DEFAULT + c->variant);
			if (!use_color)
				a = A_REVERSE;
			if (strchr(active, c->key)) {
				a = use_color ? (A_REVERSE | A_BOLD | cpair(P_BTN_DEFAULT + c->variant))
					      : (A_BOLD | A_UNDERLINE);
			}
			if (flash_key == c->key && now < flash_until)
				a = cpair(P_BTN_WARNING) | A_BOLD | A_REVERSE;
			attron(a);
			mvprintw(c->y, c->x, " [%c] %s ", c->key, c->label);
			attroff(a);
		}
	}
	(void)top;
}

static void draw(void)
{
	struct status st;
	char active[8] = { 0 };
	int n = 0, H = LINES, W = COLS, r, c;
	int btn_lines, btn_top, btn_box_y, scr_y, scr_h, rows_shown;

	parse_status(&st);
	erase();

	if (W < 60 || H < 24) {
		mvprintw(0, 0, "Terminal too small (%dx%d); need at least 60x24.", W, H);
		refresh();
		return;
	}

	/* Title bar */
	attron(cpair(P_TITLE) | A_BOLD);
	mvhline(0, 0, ' ', W);
	mvprintw(0, 1, "750RTX   %s   %s", port_name,
		 sfd >= 0 ? "connected" : "DISCONNECTED");
	attroff(cpair(P_TITLE) | A_BOLD);

	/* Status box: rows 1..5 */
	draw_box(1, 0, 5, W, "Status");
	move(2, 2);
	attron(cpair(P_LABEL));
	printw("Frequency ");
	attroff(cpair(P_LABEL));
	attron(cpair(P_FREQ) | A_BOLD);
	if (st.have_freq)
		printw("%12.3f kHz", st.freq);
	else
		printw("%12s kHz", "---.---");
	attroff(cpair(P_FREQ) | A_BOLD);
	printw("     ");
	{
		char step[16];
		size_t len = strlen(st.step);
		if (len && st.step[len - 1] == 'K')
			snprintf(step, sizeof step, "%.*s kHz", (int)(len - 1), st.step);
		else
			snprintf(step, sizeof step, "%s Hz", len ? st.step : "?");
		field("Step", step);
	}
	if (st.have_time) {
		char t[16];
		snprintf(t, sizeof t, "%d:%02d:%02d", st.hh, st.mm, st.ss);
		field("Time", t);
	}

	move(3, 2);
	if (st.have_st) {
		char buf[16];
		field("Mode", st.mode);
		field("BW", st.bw);
		field("AGC", st.agc);
		field("Power", st.pwr);
		snprintf(buf, sizeof buf, "%d", st.wpm);
		field("WPM", buf);
		snprintf(buf, sizeof buf, "%.1f", st.vol);
		field("Vol", buf);
		snprintf(buf, sizeof buf, "%d", st.err);
		field("TX err", buf);
	} else {
		printw("(no status yet - press any command key)");
	}

	move(4, 2);
	{
		double s = st.have_s && isfinite(st.s) ? st.s : 0.0;
		int i, bar;
		char sbuf[16];
		if (s < 0)
			s = 0;
		if (s > 13)
			s = 13;
		bar = (int)(s * 2);
		snprintf(sbuf, sizeof sbuf, st.have_s ? "%.1f" : "?", st.s);
		field("S", sbuf);
		for (i = 0; i < 26; i++) {
			attr_t a = i < bar ? (i < 18 ? cpair(P_GOOD) : cpair(P_ERRMSG)) | A_BOLD
					   : A_DIM;
			attron(a);
			addch(i < bar ? '#' : '.');
			attroff(a);
		}
		if (st.ovf) {
			printw("   ");
			attron(cpair(P_BAD) | A_BOLD);
			printw(" OVF ");
			attroff(cpair(P_BAD) | A_BOLD);
		}
	}

	/* Active buttons from parsed status */
	if (st.have_freq && (active[n] = map_lookup(step_map, st.step)))
		n++;
	if (st.have_st) {
		if ((active[n] = map_lookup(mode_map, st.mode)))
			n++;
		if ((active[n] = map_lookup(bw_map, st.bw)))
			n++;
		if ((active[n] = map_lookup(agc_map, st.agc)))
			n++;
		if ((active[n] = map_lookup(pwr_map, st.pwr)))
			n++;
	}
	active[n] = 0;

	/* Buttons box at the bottom, above the message line */
	btn_lines = layout_buttons(0, W);
	btn_box_y = H - 2 - (btn_lines + 2);
	btn_top = btn_box_y + 1;
	layout_buttons(btn_top, W);
	draw_box(btn_box_y, 0, btn_lines + 2, W,
		 "Commands - press the key in brackets or click");
	draw_buttons(btn_top, active);

	/* Radio screen between status and buttons */
	scr_y = 6;
	scr_h = btn_box_y - scr_y;
	if (scr_h >= 3) {
		draw_box(scr_y, 0, scr_h, W, "Radio screen");
		rows_shown = scr_h - 2;
		if (rows_shown > EROWS)
			rows_shown = EROWS;
		for (r = 0; r < rows_shown; r++) {
			for (c = 0; c < ECOLS && c + 1 < W - 1; c++) {
				attr_t a = emu_attr(efg[r][c], ebg[r][c]);
				mvaddch(scr_y + 1 + r, 1 + c, (chtype)(unsigned char)ech[r][c] | a);
			}
		}
	}

	/* Message + help lines */
	if (msg_is_error)
		attron(cpair(P_ERRMSG) | A_BOLD);
	mvprintw(H - 2, 1, "%.*s", W - 2, msg);
	if (msg_is_error)
		attroff(cpair(P_ERRMSG) | A_BOLD);
	attron(A_REVERSE);
	mvhline(H - 1, 0, ' ', W);
	mvprintw(H - 1, 1, "Ctrl+Q / F10: quit");
	attroff(A_REVERSE);

	refresh();
}

/* ------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
static void cleanup(void)
{
	endwin();
	serial_close();
}

static void send_cmd(struct cmd *c)
{
	if (sfd < 0) {
		set_msg(1, "Not connected: command '%c' not sent", c->key);
		return;
	}
	enqueue(c->key);
	set_msg(0, "Sent '%c': %s", c->key, c->label);
	flash_key = c->key;
	flash_until = now_ms() + 150;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <tty> [-b baud]\n"
		"  tty   serial device, e.g. /dev/ttyACM0\n"
		"  -b    baud rate (only matters for the UART console; default 115200)\n",
		prog);
	exit(2);
}

int main(int argc, char **argv)
{
	int i, dirty = 1;
	long long next_retry = 0, last_draw = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			port_speed = baud_to_speed(atol(argv[++i]));
			if (!port_speed) {
				fprintf(stderr, "Unsupported baud rate %s\n", argv[i]);
				return 2;
			}
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
		} else if (!port_name) {
			port_name = argv[i];
		} else {
			usage(argv[0]);
		}
	}
	if (!port_name)
		usage(argv[0]);

	signal(SIGPIPE, SIG_IGN);
	emu_clear();

	initscr();
	atexit(cleanup);
	raw();			/* get Ctrl+Q/Ctrl+C as keys */
	noecho();
	keypad(stdscr, TRUE);
	nodelay(stdscr, TRUE);
	curs_set(0);
	mousemask(BUTTON1_PRESSED | BUTTON1_CLICKED, NULL);
	mouseinterval(0);
	init_colors();
	set_msg(0, "Opening %s...", port_name);

	for (;;) {
		fd_set rf;
		struct timeval tv = { 0, 20000 };
		long long now = now_ms();
		int ch;

		/* (Re)connect */
		if (sfd < 0 && now >= next_retry) {
			sfd = serial_open();
			if (sfd >= 0) {
				set_msg(0, "Connected to %s", port_name);
				enqueue('b');	/* display on: radio redraws its status */
			} else {
				set_msg(1, "%s: %s. Retrying...", port_name, strerror(errno));
				next_retry = now + 2000;
			}
			dirty = 1;
		}

		FD_ZERO(&rf);
		FD_SET(STDIN_FILENO, &rf);
		if (sfd >= 0)
			FD_SET(sfd, &rf);
		if (select((sfd > STDIN_FILENO ? sfd : STDIN_FILENO) + 1, &rf,
			   NULL, NULL, &tv) < 0 && errno != EINTR)
			break;
		now = now_ms();

		/* Serial input */
		if (sfd >= 0 && FD_ISSET(sfd, &rf)) {
			unsigned char buf[4096];
			ssize_t n = read(sfd, buf, sizeof buf);
			if (n > 0) {
				ssize_t k;
				for (k = 0; k < n; k++)
					emu_putc(buf[k]);
				dirty = 1;
			} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
				set_msg(1, "Lost %s%s%s. Reconnecting...", port_name,
					n ? ": " : "", n ? strerror(errno) : "");
				serial_close();
				next_retry = now + 1000;
				dirty = 1;
			}
		}

		/* Keyboard / mouse */
		while ((ch = getch()) != ERR) {
			struct cmd *c;
			if (ch == 17 || ch == 3 || ch == KEY_F(10))	/* Ctrl+Q, Ctrl+C */
				return 0;
			if (ch == KEY_RESIZE) {
				dirty = 1;
				continue;
			}
			if (ch == KEY_MOUSE) {
				MEVENT ev;
				if (getmouse(&ev) == OK &&
				    (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED))) {
					for (i = 0; i < NCMDS; i++) {
						c = &cmds[i];
						if (ev.y == c->y && ev.x >= c->x &&
						    ev.x < c->x + c->w) {
							send_cmd(c);
							break;
						}
					}
				}
				dirty = 1;
				continue;
			}
			if (ch > 0 && ch < 128 && (c = find_cmd(ch)) != NULL) {
				send_cmd(c);
				dirty = 1;
			}
		}

		pump_tx(now);

		/* Redraw at most 10 times per second, plus flash timeout */
		if (flash_key && now >= flash_until) {
			flash_key = 0;
			dirty = 1;
		}
		if (dirty && now - last_draw >= 100) {
			draw();
			last_draw = now;
			dirty = 0;
		}
	}
	return 0;
}
