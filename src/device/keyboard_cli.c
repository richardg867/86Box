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
 *
 *
 * Author:	RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 RichardG.
 */
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
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/plat.h>
#include <86box/keyboard.h>
#include <86box/vid_text_render.h>
#include <86box/vnc.h>
#define KEYBOARD_CLI_DEBUG 1

/* Lookup tables for converting escape sequences to X11 keycodes. */
static const uint16_t csi_seqs[] = {
    [1]  = 0xff50, /* Home */
    [2]  = 0xff63, /* Insert */
    [3]  = 0xffff, /* Delete */
    [4]  = 0xff57, /* End */
    [5]  = 0xff55, /* Page Up */
    [6]  = 0xff56, /* Page Down */
    [11] = 0xffbe, /* F1 */
    [12] = 0xffbf, /* F2 */
    [13] = 0xffc0, /* F3 */
    [14] = 0xffc1, /* F4 */
    [15] = 0xffc2, /* F5 */
    [17] = 0xffc3, /* F6 */
    [18] = 0xffc4, /* F7 */
    [19] = 0xffc5, /* F8 */
    [20] = 0xffc6, /* F9 */
    [21] = 0xffc7, /* F10 */
    [23] = 0xffc8, /* F11 */
    [24] = 0xffc9, /* F12 */
    [25] = 0xe3be, /* F13 / Ctrl+F1 */
    [26] = 0xe3bf, /* F14 / Ctrl+F2 */
    [28] = 0xe3c0, /* F15 / Ctrl+F3 */
    [29] = 0xe3c1, /* F16 / Ctrl+F4 */
    [31] = 0xe3c2, /* F17 / Ctrl+F5 */
    [32] = 0xe3c3, /* F18 / Ctrl+F6 */
    [33] = 0xe3c4, /* F19 / Ctrl+F7 */
    [34] = 0xe3c5, /* F20 / Ctrl+F8 */
};
static const uint16_t csi_modifiers[][4] = {
    [2]  = {0xffe1, 0,      0,      0     }, /* Shift */
    [3]  = {0xffe9, 0,      0,      0     }, /* Alt */
    [4]  = {0xffe1, 0xffe9, 0,      0     }, /* Shift+Alt */
    [5]  = {0xffe3, 0,      0,      0     }, /* Ctrl */
    [6]  = {0xffe1, 0xffe3, 0,      0     }, /* Shift+Ctrl */
    [7]  = {0xffe9, 0xffe3, 0,      0     }, /* Alt+Ctrl */
    [8]  = {0xffe1, 0xffe9, 0xffe3, 0     }, /* Shift+Alt+Ctrl */
    [9]  = {0xffe7, 0,      0,      0     }, /* Meta */
    [10] = {0xffe7, 0xffe1, 0,      0     }, /* Meta+Shift */
    [11] = {0xffe7, 0xffe9, 0,      0     }, /* Meta+Alt */
    [12] = {0xffe7, 0xffe9, 0xffe1, 0     }, /* Meta+Alt+Shift */
    [13] = {0xffe1, 0xffe3, 0,      0     }, /* Meta+Ctrl */
    [14] = {0xffe1, 0xffe3, 0xffe1, 0     }, /* Meta+Ctrl+Shift */
    [15] = {0xffe1, 0xffe3, 0xffe9, 0     }, /* Meta+Ctrl+Alt */
    [16] = {0xffe1, 0xffe9, 0xffe9, 0xffe1}, /* Meta+Ctrl+Alt+Shift */
};
static const uint16_t ss3_seqs[] = {
    [' '] = 0xff80, /* Space */
    ['j'] = 0xffaa, /* * */
    ['k'] = 0xffab, /* + */
    ['l'] = 0x002c, /* , */
    ['m'] = 0xffad, /* - */
    ['A'] = 0xff52, /* Up */
    ['B'] = 0xff54, /* Down */
    ['C'] = 0xff53, /* Right */
    ['D'] = 0xff51, /* Left */
    ['E'] = 0xff58, /* Begin / Keypad 5 */
    ['F'] = 0xff57, /* End */
    ['G'] = 0xff58, /* Begin / Keypad 5 (sent by PuTTY) */
    ['H'] = 0xff50, /* Home */
    ['I'] = 0xff89, /* Tab */
    ['M'] = 0xff8d, /* Enter */
    ['P'] = 0xffbe, /* F1 */
    ['Q'] = 0xffbf, /* F2 */
    ['R'] = 0xffc0, /* F3 */
    ['S'] = 0xffc1, /* F4 */
    ['X'] = 0xffbd, /* = */
    ['Z'] = 0xe189, /* Shift+Tab (sent by PuTTY) */
};


static volatile thread_t *keyboard_cli_thread;
static event_t	*ready_event, *decrqss_event, *decrqss_ack_event;
static int	quit_escape = 0, can_decrqss = 1, in_decrqss = 0;
static char	dcs_buf[256], *decrqss_buf;


static void
keyboard_cli_send(uint16_t key, uint8_t modifier)
{
    /* Disarm quit escape sequence. */
    quit_escape = 0;

    /* Account for special keycode pages. */
    switch (key >> 8) {
	case 0xe1: /* Shift+ */
		key |= 0xff00;
		modifier = 2;
		break;

	case 0xe3: /* Ctrl+ */
		key |= 0xff00;
		modifier = 5;
		break;
    }

    /* Remove modifier if invalid. */
    if ((modifier >= (sizeof(csi_modifiers) / sizeof(csi_modifiers[0]))) ||
	!csi_modifiers[modifier][0])
	modifier = 0;

    /* Press modifier keys. */
    int i;
    if (modifier) {
	for (i = 0; i < 4; i++) {
		if (csi_modifiers[modifier][i])
			vnc_kbinput(1, csi_modifiers[modifier][i]);
	}
    }

    /* Press and release key. */
    if (key) {
	vnc_kbinput(1, key);
	vnc_kbinput(0, key);
    }

    /* Release modifier keys. */
    if (modifier) {
	for (i = 3; i >= 0; i--) {
		if (csi_modifiers[modifier][i])
			vnc_kbinput(0, csi_modifiers[modifier][i]);
	}
    }
}


static void
keyboard_cli_signal(int sig)
{
#ifdef KEYBOARD_CLI_DEBUG
    fprintf(TEXT_RENDER_OUTPUT, "\033]0;Signal: %d\a", sig);
    fflush(TEXT_RENDER_OUTPUT);
#endif

    switch (sig) {
#ifdef SIGINT
	case SIGINT: /* Ctrl+C */
		keyboard_cli_send('c', 5);
		break;
#endif
#ifdef SIGSTOP
	case SIGSTOP: /* Ctrl+S */
		keyboard_cli_send('s', 5);
		break;
#endif
#ifdef SIGTSTP
	case SIGTSTP: /* Ctrl+Z */
		keyboard_cli_send('z', 5);
		break;
#endif
#ifdef SIGQUIT
	case SIGQUIT: /* Ctrl+\ */
		keyboard_cli_send('\\', 5);
		break;
#endif
    }
}


static char
keyboard_cli_readnum(int *dest)
{
    char c;
    while (1) {
	c = getchar();
	if ((c < '0') || (c > '9'))
		break;
	*dest = (*dest * 10) + (c - '0');
    }
    return c;
}


void
keyboard_cli_process(void *priv)
{
    char c, d;
    int i, modifier;
    uint16_t code;

    thread_set_event(ready_event);

    while (1) {
	/* Read a character. */
	c = getchar();

#ifdef KEYBOARD_CLI_DEBUG
	fprintf(TEXT_RENDER_OUTPUT, "\033]0;Key: %02X\a", c);
	fflush(TEXT_RENDER_OUTPUT);
#endif

	/* Interpret the character. */
	switch (c) {
		case 0x09: /* Tab */
			keyboard_cli_send(0xff09, 0);
			break;

		case 0x01 ... 0x08: /* Ctrl+A to Ctrl+H */
		/* skip Ctrl+I (same code as Tab) */
		case 0x0a ... 0x1a: /* Ctrl+J to Ctrl+Z */
			keyboard_cli_send('`' + c, 5);
			break;

		case 0x1b: /* Escape */
			c = getchar();
			switch (c) {
				case 0x1b: /* second Escape */
					keyboard_cli_send(0xff1b, 0);
					break;

				case 'N': /* SS2 */
					/* Ignore parameter. */
					getchar();
					break;

				case '[': /* CSI */
					/* Determine if this CSI is numeric or not. */
					d = getchar();
					if ((d >= '0') && (d <= '9')) {
						/* Read numeric code. */
						i = d - '0';
						d = keyboard_cli_readnum(&i);

						/* Read numeric modifier if present. */
						modifier = 0;
						if ((d >= ':') && (d <= '?'))
							keyboard_cli_readnum(&modifier);
#ifdef KEYBOARD_CLI_DEBUG
						fprintf(TEXT_RENDER_OUTPUT, "\033]0;CSI: %c%d (mod %d)\a", c, i, modifier);
						fflush(TEXT_RENDER_OUTPUT);
#endif

						/* Determine keycode. */
						if (i < (sizeof(csi_seqs) / sizeof(csi_seqs[0])))
							code = csi_seqs[i];
						else
							code = 0;

						/* Send key. */
						if (code)
							keyboard_cli_send(code, modifier);
						break;
					}
					/* fall-through */

				case 'O': /* SS3 (or non-numeric CSI on fall-through) */
					/* Determine keycode. */
					if (c == 'O')
						d = getchar();
					if (d < (sizeof(ss3_seqs) / sizeof(ss3_seqs[0])))
						code = ss3_seqs[(uint8_t) d];
					else
						code = 0;
#ifdef KEYBOARD_CLI_DEBUG
					fprintf(TEXT_RENDER_OUTPUT, "\033]0;SS3/CSI: %c%c\a", c, d);
					fflush(TEXT_RENDER_OUTPUT);
#endif
					/* Send key. */
					if (code)
						keyboard_cli_send(code, 0);
					break;

				case 'P': /* DCS */
				case 'X': /* SOS */
				case ']': /* OSC */
				case '^': /* PM */
				case '_': /* APC */
					/* Read string until an ST sequence is found. */
					i = 0;
					while (1) {
						d = getchar();
						if ((d == '\x1b') && (getchar() == '\\')) /* ST */
							break;
						if (i < (sizeof(dcs_buf) - 1))
							dcs_buf[i++] = d;
					}
					dcs_buf[i] = '\0';
#ifdef KEYBOARD_CLI_DEBUG
					fprintf(TEXT_RENDER_OUTPUT, "\033]0;DCS: %c", c);
					for (int j = 0; j < strlen(dcs_buf); j++) {
						if ((dcs_buf[j] >= 0x20) && (dcs_buf[j] <= 0x7e))
							fputc(dcs_buf[j], TEXT_RENDER_OUTPUT);
						else
							fprintf(TEXT_RENDER_OUTPUT, "[%02X]", dcs_buf[j]);
					}
					fputc('\a', TEXT_RENDER_OUTPUT);
					fflush(TEXT_RENDER_OUTPUT);
#endif
					/* Process DECRQSS. */
					if ((c == 'P') && in_decrqss) {
						/* Allocate and copy to DECRQSS buffer. */
						decrqss_buf = malloc(strlen(dcs_buf) + 1);
						strcpy(decrqss_buf, dcs_buf);

						/* Tell the other thread that this DECRQSS is done. */
						thread_set_event(decrqss_event);

						/* Wait for acknowledgement. */
						thread_wait_event(decrqss_ack_event, -1);
						thread_reset_event(decrqss_ack_event);
					}
					break;
			}
			break;

		case 0x20 ... 0x7e: /* ASCII */
			keyboard_cli_send(c, 0);
			break;

		case 0x7f: /* Backspace */
			keyboard_cli_send(0xff08, 0);
			break;

		case EOF: /* EOF (Ctrl+Z) */
			keyboard_cli_send('z', 5);
			break;
	}
    }
}


char *
keyboard_cli_decrqss(char *query)
{
    /* Don't even try if the terminal doesn't respond to queries. (PuTTY) */
    if (!can_decrqss || in_decrqss)
	return NULL;

    /* Wait for the processing thread to be ready. */
    thread_wait_event(ready_event, -1);

    /* Send query. */
    thread_reset_event(decrqss_event);
    decrqss_buf = NULL;
    in_decrqss = 1;
    fprintf(TEXT_RENDER_OUTPUT, "\033P%s\033\\", query);
    fflush(TEXT_RENDER_OUTPUT);

    /* Wait up to 500ms for a response. */
    thread_wait_event(decrqss_event, 500);
    can_decrqss = decrqss_buf && (decrqss_buf[0] != '\0');

    /* Acknowledge to the other thread. */
    char *ret = decrqss_buf;
    in_decrqss = 0;
    thread_set_event(decrqss_ack_event);

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
    ios.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ios);
#endif

    /* Intercept signals to send their Ctrl+x sequences. */
#ifdef SIGINT
    signal(SIGINT, keyboard_cli_signal);
#endif
#ifdef SIGSTOP
    signal(SIGSTOP, keyboard_cli_signal);
#endif
#ifdef SIGTSTP
    signal(SIGTSTP, keyboard_cli_signal);
#endif
#ifdef SIGQUIT
    signal(SIGQUIT, keyboard_cli_signal);
#endif

    /* Start input processing thread. */
    ready_event = thread_create_event();
    decrqss_event = thread_create_event();
    decrqss_ack_event = thread_create_event();
    keyboard_cli_thread = thread_create(keyboard_cli_process, NULL);
}
