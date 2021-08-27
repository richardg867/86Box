/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Keyboard input for CLI mode.
 *
 *		Escape code parsing state machine based on:
 *		Williams, Paul Flo. "A parser for DEC's ANSI-compatible video
 *		terminals." VT100.net. <https://vt100.net/emu/dec_ansi_parser>
 *
 *
 *
 * Author:	RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 RichardG.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <signal.h>
#ifdef _WIN32
# include <windows.h>
# include <fcntl.h>
#else
# include <termios.h>
# include <unistd.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/plat.h>
#include <86box/keyboard.h>
#include <86box/vid_text_render.h>


/* Escape sequence parser states. */
enum {
    VT_GROUND = 0,
    VT_C3,
    VT_ESCAPE,
    VT_ESCAPE_INTERMEDIATE,
    VT_CSI_ENTRY,
    VT_CSI_IGNORE,
    VT_CSI_PARAM,
    VT_CSI_INTERMEDIATE,
    VT_DCS_ENTRY,
    VT_DCS_INTERMEDIATE,
    VT_DCS_IGNORE,
    VT_DCS_PARAM,
    VT_DCS_PASSTHROUGH,
    VT_SOS_PM_APC_STRING,
    VT_OSC_STRING
};

/* Modifier flags. */
enum {
    VT_SHIFT	= 0x01,
    VT_ALT	= 0x02,
    VT_CTRL	= 0x04,
    VT_META	= 0x08
};

/* Lookup tables for converting keys and escape sequences to keyboard scan codes. */
static const uint16_t ascii_seqs[] = {
    ['\b'] = 0x000e,
    ['\t'] = 0x000f,
    ['\n'] = 0x001c,
    [' ']  = 0x0039,
    ['!']  = 0x2a02,
    ['"']  = 0x2a28,
    ['#']  = 0x2a04,
    ['$']  = 0x2a05,
    ['%']  = 0x2a06,
    ['&']  = 0x2a08,
    ['\''] = 0x0028,
    ['(']  = 0x2a0a,
    [')']  = 0x2a0b,
    ['*']  = 0x2a09,
    ['+']  = 0x2a0d,
    [',']  = 0x0033,
    ['-']  = 0x000c,
    ['.']  = 0x0034,
    ['/']  = 0x0035,
    ['0']  = 0x000b,
    ['1']  = 0x0002,
    ['2']  = 0x0003,
    ['3']  = 0x0004,
    ['4']  = 0x0005,
    ['5']  = 0x0006,
    ['6']  = 0x0007,
    ['7']  = 0x0008,
    ['8']  = 0x0009,
    ['9']  = 0x000a,
    [':']  = 0x2a27,
    [';']  = 0x0027,
    ['<']  = 0x2a33,
    ['=']  = 0x000d,
    ['>']  = 0x2a34,
    ['?']  = 0x2a35,
    ['@']  = 0x2a03,
    ['A']  = 0x2a1e,
    ['B']  = 0x2a30,
    ['C']  = 0x2a2e,
    ['D']  = 0x2a20,
    ['E']  = 0x2a12,
    ['F']  = 0x2a21,
    ['G']  = 0x2a22,
    ['H']  = 0x2a23,
    ['I']  = 0x2a17,
    ['J']  = 0x2a24,
    ['K']  = 0x2a25,
    ['L']  = 0x2a26,
    ['M']  = 0x2a32,
    ['N']  = 0x2a31,
    ['O']  = 0x2a18,
    ['P']  = 0x2a19,
    ['Q']  = 0x2a10,
    ['R']  = 0x2a13,
    ['S']  = 0x2a1f,
    ['T']  = 0x2a14,
    ['U']  = 0x2a16,
    ['V']  = 0x2a2f,
    ['W']  = 0x2a11,
    ['X']  = 0x2a2d,
    ['Y']  = 0x2a15,
    ['Z']  = 0x2a2c,
    ['[']  = 0x001a,
    ['\\'] = 0x002b,
    [']']  = 0x001b,
    ['^']  = 0x2a07,
    ['_']  = 0x2a0c,
    ['`']  = 0x0029,
    ['a']  = 0x001e,
    ['b']  = 0x0030,
    ['c']  = 0x002e,
    ['d']  = 0x0020,
    ['e']  = 0x0012,
    ['f']  = 0x0021,
    ['g']  = 0x0022,
    ['h']  = 0x0023,
    ['i']  = 0x0017,
    ['j']  = 0x0024,
    ['k']  = 0x0025,
    ['l']  = 0x0026,
    ['m']  = 0x0032,
    ['n']  = 0x0031,
    ['o']  = 0x0018,
    ['p']  = 0x0019,
    ['q']  = 0x0010,
    ['r']  = 0x0013,
    ['s']  = 0x001f,
    ['t']  = 0x0014,
    ['u']  = 0x0016,
    ['v']  = 0x002f,
    ['w']  = 0x0011,
    ['x']  = 0x002d,
    ['y']  = 0x0015,
    ['z']  = 0x002c,
    ['{']  = 0x2a1a,
    ['|']  = 0x2a2b,
    ['}']  = 0x2a1b,
    ['~']  = 0x2a29,
    [0x7f] = 0x0053
};
static const uint16_t csi_num_seqs[] = {
    [1]  = 0xe047, /* Home */
    [2]  = 0xe052, /* Insert */
    [3]  = 0xe053, /* Delete */
    [4]  = 0xe04f, /* End */
    [5]  = 0xe049, /* Page Up */
    [6]  = 0xe051, /* Page Down */
    [11] = 0x003b, /* F1 */
    [12] = 0x003c, /* F2 */
    [13] = 0x003d, /* F3 */
    [14] = 0x003e, /* F4 */
    [15] = 0x003f, /* F5 */
    [17] = 0x0040, /* F6 */
    [18] = 0x0041, /* F7 */
    [19] = 0x0042, /* F8 */
    [20] = 0x0043, /* F9 */
    [21] = 0x0044, /* F10 */
    [23] = 0x0057, /* F11 */
    [24] = 0x0058, /* F12 */
    [25] = 0xe037, /* F13 (SysRq for Mac users) */
    [26] = 0x0046, /* F14 (Scroll Lock for Mac users) */
    [28] = 0xe11d  /* F15 (Pause for Mac users) */
};
static const uint16_t csi_letter_seqs[] = {
    [' '] = 0x0039, /* Space */
    ['j'] = 0x0037, /* * */
    ['k'] = 0x004e, /* + */
    ['l'] = 0x0033, /* , */
    ['m'] = 0x004a, /* - */
    ['A'] = 0xe048, /* Up */
    ['B'] = 0xe050, /* Down */
    ['C'] = 0xe04d, /* Right */
    ['D'] = 0xe04b, /* Left */
    ['F'] = 0xe04f, /* End */
    ['H'] = 0xe047, /* Home */
    ['I'] = 0x000f, /* Tab */
    ['M'] = 0x001c, /* Enter */
    ['P'] = 0x003b, /* F1 */
    ['Q'] = 0x003c, /* F2 */
    ['R'] = 0x003d, /* F3 */
    ['S'] = 0x003e, /* F4 */
    ['X'] = 0x000d, /* = */
    ['Z'] = 0x2a0f, /* Shift+Tab */
};
static const uint8_t csi_modifiers[] = {
    [2]  = VT_SHIFT,
    [3]  = VT_ALT,
    [4]  = VT_SHIFT | VT_ALT,
    [5]  = VT_CTRL,
    [6]  = VT_SHIFT | VT_CTRL,
    [7]  = VT_ALT | VT_CTRL,
    [8]  = VT_SHIFT | VT_ALT | VT_CTRL,
    [9]  = VT_META,
    [10] = VT_META | VT_SHIFT,
    [11] = VT_META | VT_ALT,
    [12] = VT_META | VT_ALT | VT_SHIFT,
    [13] = VT_META | VT_CTRL,
    [14] = VT_META | VT_CTRL | VT_SHIFT,
    [15] = VT_META | VT_CTRL | VT_ALT,
    [16] = VT_META | VT_CTRL | VT_ALT | VT_SHIFT
};


static volatile thread_t *keyboard_cli_thread;
static event_t	*ready_event, *decrqss_event, *decrqss_ack_event;
static int	param_buf_pos = 0, collect_buf_pos = 0, dcs_buf_pos = 0, osc_buf_pos = 0,
		can_decrqss = 1, in_decrqss = 0;
static char	param_buf[32], collect_buf[32], dcs_buf[32], osc_buf[32],
		*decrqss_buf;

#define ENABLE_CLI_KEYBOARD_LOG 1
#ifdef ENABLE_CLI_KEYBOARD_LOG
int cli_keyboard_do_log = ENABLE_CLI_KEYBOARD_LOG;

static void
cli_keyboard_log(const char *fmt, ...)
{
    va_list ap;

    if (cli_keyboard_do_log) {
	va_start(ap, fmt);
	pclog_ex(fmt, ap);
	va_end(ap);
    }
}

static void
cli_keyboard_log_key(const char *func, int c)
{
    if ((c >= 0x20) && (c <= 0x7e))
	cli_keyboard_log("CLI Keyboard: %s(%c)\n", func, c);
    else
	cli_keyboard_log("CLI Keyboard: %s(%02X)\n", func, c);
}
#else
#define cli_keyboard_log(fmt, ...)
#define cli_keyboard_log_key(func, c)
#endif


static void
keyboard_cli_send(uint16_t code, uint8_t modifier)
{
    cli_keyboard_log("CLI Keyboard: send(%04X, %d)\n", code, modifier);

    /* Determine modifier scancodes to be pressed as well. */
    if (modifier < sizeof(csi_modifiers))
	modifier = csi_modifiers[modifier];
    else
	modifier = 0;

    /* Determine modifiers set by the keycode definition. */
    switch (code >> 8) {
	case 0x1d:
		modifier |= VT_CTRL;
		break;

	case 0x2a:
		modifier |= VT_SHIFT;
		break;

	case 0x38:
		modifier |= VT_ALT;
		break;

	case 0x5b:
		modifier |= VT_META;
		break;
    }

    /* Handle special cases. */
    switch (code) {
	case 0xe037: /* SysRq */
		if (modifier & (VT_SHIFT | VT_CTRL)) {
			modifier &= ~(VT_SHIFT | VT_CTRL);
		} else if (modifier & VT_ALT) {
			modifier &= ~VT_ALT;
			code = 0x0054;
		} else {
			if (modifier & VT_META)
				keyboard_input(1, 0xe05b);
			keyboard_input(1, 0xe02a);
			keyboard_input(1, 0xe037);
			keyboard_input(0, 0xe037);
			keyboard_input(0, 0xe02a);
			if (modifier & VT_META)
				keyboard_input(0, 0xe05b);
		}
		return;

	case 0xe11d: /* Pause */
		if (modifier & VT_CTRL) {
			modifier &= ~VT_CTRL;
			code = 0xe046;
		}
		break;
    }

    /* Press modifier keys. */
    if (modifier & VT_META)
	keyboard_input(1, 0xe05b);
    if (modifier & VT_CTRL)
	keyboard_input(1, 0x001d);
    if (modifier & VT_ALT)
	keyboard_input(1, 0x0038);
    if (modifier & VT_SHIFT)
	keyboard_input(1, 0x002a);

    /* Press and release key. */
    if (code) {
	keyboard_input(1, code);
	keyboard_input(0, code);
    }

    /* Release modifier keys. */
    if (modifier & VT_SHIFT)
	keyboard_input(0, 0x002a);
    if (modifier & VT_ALT)
	keyboard_input(0, 0x0038);
    if (modifier & VT_CTRL)
	keyboard_input(0, 0x001d);
    if (modifier & VT_META)
	keyboard_input(0, 0xe05b);
}


static void
keyboard_cli_clear(int c)
{
    cli_keyboard_log_key("clear", c);

    collect_buf_pos = param_buf_pos = 0;
    collect_buf[0] = param_buf[0] = '\0';
}


static void
keyboard_cli_collect(int c)
{
    cli_keyboard_log_key("collect", c);

    if (collect_buf_pos < (sizeof(collect_buf) - 1)) {
	collect_buf[collect_buf_pos++] = c;
	collect_buf[collect_buf_pos] = '\0';
    }
}


static void
keyboard_cli_csi_dispatch(int c)
{
    cli_keyboard_log_key("csi_dispatch", c);

    /* Discard an invalid sequence with no letter or numeric code. */
    if ((c == '~') && (param_buf_pos < 1))
	return;

    /* Read numeric code and modifier parameters if applicable. */
    int code = 0, modifier = 0;
    char delimiter;
    switch (sscanf(param_buf, "%d%c%d", &code, &delimiter, &modifier)) {
	case EOF:
	case 0:
		code = 0;
		/* fall-through */

	case 1 ... 2:
		modifier = 0;
		break;
    }

    /* Determine keycode. */
    if (c == '~')
	code = csi_num_seqs[code];
    else
	code = csi_letter_seqs[c];

    /* Press key with modifier. */
    keyboard_cli_send(code, modifier);
}


static void
keyboard_cli_esc_dispatch(int c)
{
    cli_keyboard_log_key("esc_dispatch", c);

    switch (collect_buf[0]) {
	case 'M': /* special: menu */
		cli_menu = 1;
		break;

	case 'O': /* SS3 */
		/* Handle as a CSI with no parameters. */
		keyboard_cli_csi_dispatch(c);
		break;
    }
}


static void
keyboard_cli_execute(int c)
{
    cli_keyboard_log_key("execute", c);

    switch (c) {
	case 0x01 ... 0x08: /* Ctrl+A to Ctrl+H */
	/* skip Ctrl+I (Tab), Ctrl+J (Enter) */
	case 0x0b ... 0x1a: /* Ctrl+K to Ctrl+Z */
		keyboard_cli_send(ascii_seqs['`' + c], 5);
		break;

	case 0x09: /* Tab */
	case 0x0a: /* Enter */
		keyboard_cli_send(ascii_seqs[c], 0);
		break;
    }
}


static void
keyboard_cli_hook(int c)
{
    cli_keyboard_log_key("hook", c);

    /* Initialize DCS buffer. */
    dcs_buf_pos = 0;
    dcs_buf[dcs_buf_pos++] = c;
    dcs_buf[dcs_buf_pos] = '\0';
}


static void
keyboard_cli_put(int c)
{
    cli_keyboard_log_key("put", c);

    /* Append character to DCS buffer. */
    if (dcs_buf_pos < (sizeof(dcs_buf) - 1)) {
	dcs_buf[dcs_buf_pos++] = c;
	dcs_buf[dcs_buf_pos] = '\0';
    }
}


static void
keyboard_cli_unhook(int c)
{
    cli_keyboard_log_key("unhook", c);

    /* Process DECRQSS. */
    if ((collect_buf[0] == '$') && (dcs_buf[0] == 'r') && in_decrqss) {
	/* Allocate and copy to DECRQSS buffer. */
	char *p = decrqss_buf = malloc(strlen(dcs_buf) + 1);
	if (param_buf[0] != '\0')
		p += sprintf(p, "%c", param_buf[0]);
	sprintf(p, "%c%s", collect_buf[0], dcs_buf);

	/* Tell the other thread that this DECRQSS is done. */
	thread_set_event(decrqss_event);

	/* Wait for acknowledgement. */
	thread_wait_event(decrqss_ack_event, -1);
	thread_reset_event(decrqss_ack_event);
    }
}


static void
keyboard_cli_osc_start(int c)
{
    cli_keyboard_log_key("osc_start", c);

    /* Initialize OSC buffer. */
    osc_buf_pos = 0;
    osc_buf[0] = '\0';
}


static void
keyboard_cli_osc_put(int c)
{
    cli_keyboard_log_key("osc_put", c);

    /* Append character to OSC buffer. */
    if (osc_buf_pos < (sizeof(osc_buf) - 1)) {
	osc_buf[osc_buf_pos++] = c;
	osc_buf[osc_buf_pos] = '\0';
    }
}


static void
keyboard_cli_osc_end(int c)
{
    cli_keyboard_log_key("osc_end", c);
}


static void
keyboard_cli_param(int c)
{
    cli_keyboard_log_key("param", c);

    if (param_buf_pos < (sizeof(param_buf) - 1)) {
	param_buf[param_buf_pos++] = c;
	param_buf[param_buf_pos] = '\0';
    }
}


void
keyboard_cli_process(void *priv)
{
    int c = 0, state = VT_GROUND, prev_state = VT_GROUND;

    /* Flag thread as ready. */
    thread_set_event(ready_event);

    /* Run state machine loop. */
    while (1) {
	/* Handle state exits. */
	if ((prev_state == VT_DCS_PASSTHROUGH) && (state != VT_DCS_PASSTHROUGH))
		keyboard_cli_unhook(c);
	else if ((prev_state == VT_OSC_STRING) && (state != VT_OSC_STRING))
		keyboard_cli_osc_end(c);
	prev_state = state;

	/* Read character. */
	c = getchar();

	/* Interpret conditions for any state. */
	switch (c) {
		case 0x1b:
			/* Interpret ESC ESC as escaped ESC. Note that some terminals
			   may emit extended codes prefixed with ESC ESC, but there's
			   not much we can do to parse those. */
			if (state == VT_ESCAPE) {
				keyboard_cli_send(0x0001, 0);
				state = VT_GROUND;
			} else {
				state = VT_ESCAPE;
			}
			continue;

		case 0x7f:
			/* Ignore, unless this is an user-initiated Backspace. */
			if (state == VT_GROUND)
				break;
			else
				continue;
	}

	/* Interpret conditions for specific states. */
	switch (state) {
		case VT_GROUND:
			switch (c) {
				case 0x00 ... 0x1f:
					keyboard_cli_execute(c);
					continue;

				case 0x20 ... 0x7e: /* ASCII */
					keyboard_cli_send(ascii_seqs[c], 0);
					break;

				case 0x7f: /* Backspace */
					keyboard_cli_send(ascii_seqs['\b'], 0);
					break;

				case 0xc3:
					state = VT_C3;
					break;

				case EOF: /* EOF (Ctrl+Z) */
					keyboard_cli_send(ascii_seqs['z'], 5);
					break;
			}
			break;

		case VT_C3:
			switch (c) {
				case 0x81 ... 0x9a: /* Alt+Shift+A to Alt+Shift+Z (xterm) */
				case 0xa1 ... 0xba: /* Alt+A to Alt+Z (xterm) */
					keyboard_cli_send(ascii_seqs['`' + (c & 0x1f)], (c >= 0xa1) ? 3 : 4);
					break;

				case 0xa0: /* Alt+Space (xterm) */
					keyboard_cli_send(ascii_seqs[' '], 3);
					break;
			}
			state = VT_GROUND;
			break;

		case VT_ESCAPE:
			switch (c) {
				case 0x00 ... 0x1f:
					keyboard_cli_execute(c);
					continue;

				case 0x20 ... 0x2f:
				case 0x4f:
					keyboard_cli_clear(c);
					keyboard_cli_collect(c);
					state = VT_ESCAPE_INTERMEDIATE;
					break;

				case 0x30 ... 0x4e:
				case 0x51 ... 0x57:
				case 0x59:
				case 0x5a:
				case 0x5c:
					keyboard_cli_esc_dispatch(c);
					state = VT_GROUND;
					break;

				case 0x50:
					state = VT_DCS_ENTRY;
					keyboard_cli_clear(c);
					break;

				case 0x58:
				case 0x5e:
				case 0x5f:
					state = VT_SOS_PM_APC_STRING;
					break;

				case 0x5b:
					state = VT_CSI_ENTRY;
					keyboard_cli_clear(c);
					break;

				case 0x5d:
					state = VT_OSC_STRING;
					keyboard_cli_osc_start(c);
					break;
			}
			break;

		case VT_ESCAPE_INTERMEDIATE:
			switch (c) {
				case 0x00 ... 0x1a:
				case 0x1c ... 0x1f:
					keyboard_cli_execute(c);
					continue;

				case 0x20 ... 0x2f:
					keyboard_cli_collect(c);
					break;

				case 0x30 ... 0x7e:
					keyboard_cli_esc_dispatch(c);
					state = VT_GROUND;
					break;
			}
			break;

		case VT_CSI_ENTRY:
			switch (c) {
				case 0x00 ... 0x1a:
				case 0x1c ... 0x1f:
					keyboard_cli_execute(c);
					continue;

				case 0x20 ... 0x2f:
					keyboard_cli_collect(c);
					state = VT_ESCAPE_INTERMEDIATE;
					break;

				case 0x30 ... 0x39:
				case 0x3b:
					keyboard_cli_param(c);
					state = VT_CSI_PARAM;
					break;

				case 0x3a:
					state = VT_CSI_IGNORE;
					break;

				case 0x3c ... 0x3f:
					keyboard_cli_collect(c);
					state = VT_CSI_PARAM;
					break;

				case 0x40 ... 0x7e:
					keyboard_cli_csi_dispatch(c);
					state = VT_GROUND;
					break;
			}
			break;

		case VT_CSI_IGNORE:
			switch (c) {
				case 0x00 ... 0x1a:
				case 0x1c ... 0x1f:
					keyboard_cli_execute(c);
					continue;

				case 0x40 ... 0x7e:
					state = VT_GROUND;
					break;
			}
			break;

		case VT_CSI_PARAM:
			switch (c) {
				case 0x00 ... 0x1a:
				case 0x1c ... 0x1f:
					keyboard_cli_execute(c);
					continue;

				case 0x20 ... 0x2f:
					keyboard_cli_collect(c);
					state = VT_CSI_INTERMEDIATE;
					break;

				case 0x30 ... 0x39:
				case 0x3b:
					keyboard_cli_param(c);
					break;

				case 0x3a:
				case 0x3c ... 0x3f:
					state = VT_CSI_IGNORE;
					break;

				case 0x40 ... 0x7e:
					keyboard_cli_csi_dispatch(c);
					state = VT_GROUND;
					break;
			}
			break;

		case VT_CSI_INTERMEDIATE:
			switch (c) {
				case 0x00 ... 0x1a:
				case 0x1c ... 0x1f:
					keyboard_cli_execute(c);
					continue;

				case 0x20 ... 0x2f:
					keyboard_cli_collect(c);
					break;

				case 0x30 ... 0x3f:
					state = VT_CSI_IGNORE;
					break;

				case 0x40 ... 0x7e:
					keyboard_cli_csi_dispatch(c);
					state = VT_GROUND;
					break;
			}
			break;

		case VT_DCS_ENTRY:
			if (prev_state != VT_DCS_ENTRY)
				keyboard_cli_clear(c);
			switch (c) {
				case 0x20 ... 0x2f:
					keyboard_cli_collect(c);
					state = VT_DCS_INTERMEDIATE;
					break;

				case 0x30 ... 0x39:
				case 0x3b:
					keyboard_cli_param(c);
					state = VT_DCS_PARAM;
					break;

				case 0x3a:
					state = VT_DCS_IGNORE;
					break;

				case 0x3c ... 0x3f:
					keyboard_cli_collect(c);
					state = VT_DCS_PARAM;
					break;

				case 0x40 ... 0x7e:
					state = VT_DCS_PASSTHROUGH;
					keyboard_cli_hook(c);
					break;
			}
			break;

		case VT_DCS_INTERMEDIATE:
			switch (c) {
				case 0x20 ... 0x2f:
					keyboard_cli_collect(c);
					break;

				case 0x30 ... 0x3f:
					state = VT_DCS_IGNORE;
					break;

				case 0x40 ... 0x7e:
					state = VT_DCS_PASSTHROUGH;
					keyboard_cli_hook(c);
					break;
			}
			break;

		case VT_DCS_PARAM:
			switch (c) {
				case 0x20 ... 0x2f:
					keyboard_cli_collect(c);
					state = VT_DCS_INTERMEDIATE;
					break;

				case 0x30 ... 0x39:
				case 0x3b:
					keyboard_cli_param(c);
					break;

				case 0x3a:
				case 0x3c ... 0x3f:
					state = VT_DCS_IGNORE;
					break;

				case 0x40 ... 0x7e:
					state = VT_DCS_PASSTHROUGH;
					keyboard_cli_hook(c);
					break;
			}
			break;

		case VT_DCS_PASSTHROUGH:
			switch (c) {
				case 0x00 ... 0x7e:
					keyboard_cli_put(c);
					break;
			}
			break;

		case VT_OSC_STRING:
			switch (c) {
				case 0x20 ... 0x7e:
					keyboard_cli_osc_put(c);
					break;
			}
			break;
	}
    }
}


char *
keyboard_cli_decrqss(char *query)
{
    /* Don't query if DECRQSS queries are disabled,
       or we're in the middle of another query. */
    if (!can_decrqss || in_decrqss) {
	cli_keyboard_log("CLI Keyboard: decrqss(%s) ignored\n", query);
	return NULL;
    }
    cli_keyboard_log("CLI Keyboard: decrqss(%s)\n", query);

    /* Wait for the processing thread to be ready. */
    thread_wait_event(ready_event, -1);

    /* Flag that we're in a query. */
    thread_reset_event(decrqss_event);
    decrqss_buf = NULL;
    in_decrqss = 1;

    /* Send query. */
    char *full_query = malloc(strlen(query) + 5);
    sprintf(full_query, "\033P%s\033\\", query);
    cli_render_write(full_query);

    /* Wait up to 500ms for a response. */
    thread_wait_event(decrqss_event, 500);

    /* Determine if the terminal responded. If that's not the case and this
       is the first query, disable DECRQSS queries altogether, to prevent
       constant timeouts on terminals that never respond (PuTTY and others). */
    if (can_decrqss == 1)
	can_decrqss = (decrqss_buf && (decrqss_buf[0] != '\0')) << 1;

    /* Acknowledge to the other thread. */
    char *ret = decrqss_buf;
    in_decrqss = 0;
    thread_set_event(decrqss_ack_event);
    cli_keyboard_log("CLI Keyboard: decrqss(%s) = %s\n", query, ret);

    /* Don't forget to free! */
    return ret;
}


int
keyboard_cli_decrqss_str(char *query, char *substring)
{
    /* Perform DECRQSS query. */
    char *buf = keyboard_cli_decrqss(query);
    if (!buf)
	return -3;

    /* Compare length. */
    int len = strlen(buf), i;
    if (len < strlen(substring)) {
	i = -2;
	goto end;
    }

    /* Convert all parameter delimiters to colon. */
    for (i = 0; i < len; i++) {
	if ((buf[i] > ':') && (buf[i] <= '?'))
		buf[i] = ':';
    }

    /* Check substring. */
    char *p = strstr(buf, substring);
    if (p)
	i = p - buf;
    else
	i = -1;

end:
    /* Clean up and return result. */
    free(buf);
    return i;
}


void
keyboard_cli_init()
{
    /* Enable raw input. */
#ifdef _WIN32
    HANDLE h;
    FILE *fp = NULL;
    int i;
    if ((h = GetStdHandle(STD_INPUT_HANDLE)) != NULL) {
	/* We got the handle, now open a file descriptor. */
	SetConsoleMode(h, 0);
	if ((i = _open_osfhandle((intptr_t)h, _O_TEXT)) != -1) {
		/* We got a file descriptor, now allocate a new stream. */
		if ((fp = _fdopen(i, "r")) != NULL) {
			/* Got the stream, re-initialize stdin without it. */
			(void)freopen("CONIN$", "r", stdin);
			setvbuf(stdin, NULL, _IONBF, 0);
		}
	}
    }
    if (fp != NULL) {
	fclose(fp);
	fp = NULL;
    }
#else
    struct termios ios;
    tcgetattr(STDIN_FILENO, &ios);
    ios.c_lflag &= ~(ECHO | ICANON | ISIG);
    ios.c_iflag &= ~IXON;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ios);
#endif

    /* Start input processing thread. */
    ready_event = thread_create_event();
    decrqss_event = thread_create_event();
    decrqss_ack_event = thread_create_event();
    keyboard_cli_thread = thread_create(keyboard_cli_process, NULL);
}
