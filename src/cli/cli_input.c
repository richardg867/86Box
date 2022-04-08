/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		ANSI input module for the command line interface.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    include <fcntl.h>
#    include <windows.h>
#else
#    include <errno.h>
#    include <termios.h>
#    include <unistd.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/cli.h>
#include <86box/device.h>
#include <86box/keyboard.h>
#include <86box/plat.h>

/* Escape sequence parser states. */
enum {
    VT_GROUND = 0,
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

/* Lookup tables for converting keys and escape sequences to keyboard scan codes. */
const uint16_t ascii_seqs[128] = {
    ['\b'] = 0x000e,
    ['\t'] = 0x000f,
    ['\n'] = 0x001c,
    ['\r'] = 0x001c,
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

static int  in_raw = 0, param_buf_pos = 0, collect_buf_pos = 0, dcs_buf_pos = 0, osc_buf_pos = 0;
static char param_buf[32], collect_buf[32], dcs_buf[32], osc_buf[32];
#ifdef _WIN32
static DWORD saved_console_mode = 0;
#else
static tcflag_t saved_lflag = 0, saved_iflag = 0;
#endif

#define ENABLE_CLI_INPUT_LOG 1
#ifdef ENABLE_CLI_INPUT_LOG
int cli_input_do_log = ENABLE_CLI_INPUT_LOG;

static void
cli_input_log(const char *fmt, ...)
{
    va_list ap;

    if (cli_input_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}

static void
cli_input_log_key(const char *func, int c)
{
    if ((c >= 0x20) && (c <= 0x7e))
        cli_input_log("CLI Input: %s(%c)\n", func, c);
    else
        cli_input_log("CLI Input: %s(%02X)\n", func, c);
}
#else
#    define cli_input_log(fmt, ...)
#    define cli_input_log_key(func, c)
#endif

void
cli_input_send(uint16_t code, uint8_t modifier)
{
    cli_input_log("CLI Input: send(%04X, %02X)\n", code, modifier);

    /* Add modifiers set by the keycode definition. */
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
                modifier |= VT_SHIFT_FAKE;
            }
            break;

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
    if (modifier & VT_SHIFT_FAKE)
        keyboard_input(1, 0xe02a);

    /* Press and release key. */
    if (code) {
        keyboard_input(1, code);
        keyboard_input(0, code);
    }

    /* Release modifier keys. */
    if (modifier & VT_SHIFT_FAKE)
        keyboard_input(0, 0xe02a);
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
cli_input_raw()
{
    /* Don't do anything if raw input is already enabled. */
    if (in_raw)
        return;
    in_raw = 1;

#ifdef _WIN32
    /* Enable ANSI input. */
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h) {
        /* Save existing mode for restoration purposes. */
        if (GetConsoleMode(h, &saved_console_mode))
            in_raw = 2;
        else
            cli_input_log("CLI Input: GetConsoleMode failed (%08X)\n", GetLastError());

        /* Set new mode. */
        if (!SetConsoleMode(h, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS)) /* ENABLE_EXTENDED_FLAGS disables quickedit */
            cli_input_log("CLI Input: SetConsoleMode failed (%08X)\n", GetLastError());
    } else {
        cli_input_log("CLI Input: GetStdHandle failed (%08X)\n", GetLastError());
    }
#else
    /* Enable raw input. */
    struct termios ios;
    if (tcgetattr(STDIN_FILENO, &ios)) {
        cli_input_log("CLI Input: tcgetattr failed (%d)\n", errno);
    } else {
        /* Save existing flags for restoration purposes. */
        in_raw      = 2;
        saved_lflag = ios.c_lflag;
        saved_iflag = ios.c_iflag;

        /* Set new flags. */
        ios.c_lflag &= ~(ECHO | ICANON | ISIG);
        ios.c_iflag &= ~IXON;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ios))
            cli_input_log("CLI Input: tcsetattr failed (%d)\n", errno);
    }
#endif
}

static void
cli_input_unraw()
{
    /* Don't do anything if raw input is not enabled. */
    if (!in_raw)
        return;

    /* Restore saved terminal state. */
    if (in_raw == 2) {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h) {
            if (!SetConsoleMode(h, saved_console_mode))
                cli_input_log("CLI Input: SetConsoleMode failed (%08X)\n", GetLastError());
        } else {
            cli_input_log("CLI Input: GetStdHandle failed (%08X)\n", GetLastError());
        }
#else
        struct termios ios;
        if (tcgetattr(STDIN_FILENO, &ios)) {
            cli_input_log("CLI Input: tcgetattr failed (%d)\n", errno);
        } else {
            ios.c_lflag = saved_lflag;
            ios.c_iflag = saved_iflag;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ios))
                cli_input_log("CLI Input: tcsetattr failed (%d)\n", errno);
        }
#endif
    }

    in_raw = 0;
}

static int
cli_input_response_strstr(char *response, const char *cmp)
{
    /* Allocate a local buffer. */
    int len = strlen(response);
    if (!len)
        return -1;
    char *response_cleaned = malloc(len + 2),
         *p                = response_cleaned, ch;

    /* Copy response while removing double colons. */
    for (int i = 0; i < len; i++) {
        ch = response[i];
        if ((ch >= ':') && (ch <= '?')) {
            if ((p != response_cleaned) && (*(p - 1) != ':'))
                *p++ = ':';
        } else {
            *p++ = ch;
        }
    }

    /* Replace non-numeric first character with a colon. */
    ch = response_cleaned[0];
    if ((ch < '0') || (ch > '9'))
        response_cleaned[0] = ':';

    /* Add or replace last character with a colon. */
    ch = *(p - 1);
    if ((ch >= '0') && (ch <= '9'))
        *p++ = ':';
    else
        *(p - 1) = ':';
    *p = '\0';

    /* Perform comparison. */
    p = strstr(response_cleaned, cmp);
    free(response_cleaned);
    return !!p;
}

static void
cli_input_clear(int c)
{
    cli_input_log_key("clear", c);

    collect_buf_pos = param_buf_pos = 0;
    collect_buf[0] = param_buf[0] = '\0';
}

static void
cli_input_collect(int c)
{
    cli_input_log_key("collect", c);

    if (collect_buf_pos < (sizeof(collect_buf) - 1)) {
        collect_buf[collect_buf_pos++] = c;
        collect_buf[collect_buf_pos]   = '\0';
    }
}

static void
cli_input_csi_dispatch(int c)
{
    cli_input_log_key("csi_dispatch", c);

    /* Discard an invalid sequence with no letter or numeric code. */
    if ((c == '~') && (param_buf_pos < 1))
        return;

    /* Read numeric code and modifier parameters if applicable. */
    int  code, modifier;
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

    /* Determine if this is actually a terminal size query response. */
    if (cli_term.cpr && (c == 'R') && (modifier > 1)) {
        if (code == 1) {
            cli_term.cpr &= ~2;

            /* If we're exactly one character in, we can assume the
               terminal has interpreted our UTF-8 sequence as UTF-8. */
            cli_term.can_utf8 = modifier == 2;
            cli_input_log("CLI Input: CPR probe reports%sUTF-8\n", cli_term.can_utf8 ? " " : " no ");
        } else {
            cli_term.cpr &= ~1;

            /* Set 0-based terminal size to the current 1-based cursor position. */
            cli_term_setsize(modifier, code, "CPR");
        }
        return;
    }

    /* Determine if this is actually a device attribute query response. */
    if (cli_term.sda && (c == 'c') && (collect_buf[0] == '?')) {
        cli_input_log("CLI Input: Attributes[%d] report:", cli_term.sda);
        if (cli_term.sda == 1) { /* primary attributes */
            /* Enable sixel graphics if supported. */
            modifier = cli_input_response_strstr(param_buf, ":4:");
            cli_input_log("%ssixel,", modifier ? " " : " no ");
            if (modifier)
                cli_term.gfx_level |= TERM_GFX_SIXEL;
            else
                cli_term.gfx_level &= ~TERM_GFX_SIXEL;

            /* Enable 16-bit color if supported. */
            modifier = cli_input_response_strstr(param_buf, ":22:");
            cli_input_log("%scolor\n", modifier ? " " : " no ");
            if ((cli_term.color_level < TERM_COLOR_4BIT) && modifier)
                cli_term_setcolor(TERM_COLOR_4BIT, "attributes");
        } else {
            cli_input_log("nothing we care about\n");
        }
        cli_term.sda = 0;
        return;
    }

    /* Determine keycode. */
    if (c == '~') {
        if ((code >= 0) && (code < (sizeof(csi_num_seqs) / sizeof(csi_num_seqs[0]))))
            code = csi_num_seqs[code];
        else
            code = 0;
    } else {
        if ((code >= 0) && (code < (sizeof(csi_letter_seqs) / sizeof(csi_letter_seqs[0]))))
            code = csi_letter_seqs[c];
        else
            code = 0;
    }

    /* Determine modifiers. */
    if ((modifier >= 0) && (modifier < sizeof(csi_modifiers)))
        modifier = csi_modifiers[modifier];
    else
        modifier = 0;

    /* Press key with any modifiers. */
    cli_input_send(code, modifier);
}

static void
cli_input_esc_dispatch(int c)
{
    cli_input_log_key("esc_dispatch", c);

    switch (collect_buf[0]) {
        case '\0': /* no parameter */
            switch (c) {
                case 0x20 ... 0x7f: /* Alt+Space to Alt+Backspace */
                    cli_input_send(ascii_seqs[c], VT_ALT);
                    break;
            }
            break;

        case 'O': /* SS3 */
            /* Handle as a CSI with no parameters. */
            cli_input_csi_dispatch(c);
            break;
    }
}

static void
cli_input_execute(int c)
{
    cli_input_log_key("execute", c);

    switch (c) {
        case 0x01 ... 0x08: /* Ctrl+A to Ctrl+H */
        /* skip Ctrl+I (Tab), Ctrl+J (Enter) */
        case 0x0b ... 0x0c: /* Ctrl+K to Ctrl+L */
        /* skip Ctrl+M (Windows Enter) */
        case 0x0e ... 0x1a: /* Ctrl+N to Ctrl+Z */
            cli_input_send(ascii_seqs['`' + c], VT_CTRL);
            break;

        case 0x09: /* Tab */
        case 0x0a: /* Enter */
            cli_input_send(ascii_seqs[c], 0);
            break;

        case 0x0d: /* Enter (Windows) */
            cli_input_send(ascii_seqs['\n'], 0);
            break;
    }
}

static void
cli_input_hook(int c)
{
    cli_input_log_key("hook", c);

    /* Initialize DCS buffer. */
    dcs_buf_pos            = 0;
    dcs_buf[dcs_buf_pos++] = c;
    dcs_buf[dcs_buf_pos]   = '\0';
}

static void
cli_input_put(int c)
{
    cli_input_log_key("put", c);

    /* Append character to DCS buffer. */
    if (dcs_buf_pos < (sizeof(dcs_buf) - 1)) {
        dcs_buf[dcs_buf_pos++] = c;
        dcs_buf[dcs_buf_pos]   = '\0';
    }
}

static void
cli_input_unhook(int c)
{
    cli_input_log_key("unhook", c);

    /* Process DECRQSS. */
    if ((collect_buf[0] == '$') && (dcs_buf[0] == 'r')) {
        cli_input_log("CLI Input: DECRQSS response: %s\n", dcs_buf);

        /* Interpret color-related responses. */
        char last_char = dcs_buf[strlen(dcs_buf) - 1];
        switch (last_char) {
            case 'm':
                /* Stop if this was a spurious response. */
                if (!cli_term.decrqss_color)
                    break;

                /* Interpret response according to the color level currently being queried. */
                switch (cli_term.decrqss_color) {
                    case TERM_COLOR_24BIT:
                        if (cli_input_response_strstr(dcs_buf, ":2:1:2:3:")) {
                            /* 24-bit color supported. */
                            cli_term_setcolor(TERM_COLOR_24BIT, "DECRQSS");
                        } else if (cli_term.color_level < TERM_COLOR_8BIT) {
                            /* Try 8-bit color if we don't explicitly know it's supported. */
                            cli_term.decrqss_color = TERM_COLOR_8BIT;
                            cli_render_write(RENDER_SIDEBAND_DECRQSS_COLOR, "\033[38;5;255m\033P$qm\033\\\033[0m");
                            break;
                        }
                        cli_term.decrqss_color = TERM_COLOR_NONE;
                        break;

                    case TERM_COLOR_8BIT:
                        if (cli_input_response_strstr(dcs_buf, ":5:255:")) {
                            /* 8-bit color supported. */
                            cli_term_setcolor(TERM_COLOR_8BIT, "DECRQSS");
                        } else if (cli_term.color_level < TERM_COLOR_4BIT) {
                            /* Try 4-bit color if we don't explicitly know it's supported. */
                            cli_term.decrqss_color = TERM_COLOR_4BIT;
                            cli_render_write(RENDER_SIDEBAND_DECRQSS_COLOR, "\033[97m\033P$qm\033\\\033[0m");
                            break;
                        }
                        cli_term.decrqss_color = TERM_COLOR_NONE;
                        break;

                    case TERM_COLOR_4BIT:
                        if (cli_input_response_strstr(dcs_buf, ":97:")) {
                            /* 4-bit color supported. */
                            cli_term_setcolor(TERM_COLOR_4BIT, "DECRQSS");
                        }
                        cli_term.decrqss_color = TERM_COLOR_NONE;
                        break;
                }
                break;

            case 'q':
                /* Save current cursor style. */
                if (sscanf(&dcs_buf[1], "%d", &cli_term.decrqss_cursor) != 1)
                    cli_term.decrqss_cursor = 0;
                cli_input_log("CLI Input: DECRQSS reports a cursor style of %d\n", cli_term.decrqss_cursor);
                break;
        }
    }
}

static void
cli_input_osc_start(int c)
{
    cli_input_log_key("osc_start", c);

    /* Initialize OSC buffer. */
    osc_buf_pos = 0;
    osc_buf[0]  = '\0';
}

static void
cli_input_osc_put(int c)
{
    cli_input_log_key("osc_put", c);

    /* Append character to OSC buffer. */
    if (osc_buf_pos < (sizeof(osc_buf) - 1)) {
        osc_buf[osc_buf_pos++] = c;
        osc_buf[osc_buf_pos]   = '\0';
    }
}

static void
cli_input_osc_end(int c)
{
    cli_input_log_key("osc_end", c);
}

static void
cli_input_param(int c)
{
    cli_input_log_key("param", c);

    if (param_buf_pos < (sizeof(param_buf) - 1)) {
        param_buf[param_buf_pos++] = c;
        param_buf[param_buf_pos]   = '\0';
    }
}

void
cli_input_process(void *priv)
{
    int c = 0, state = VT_GROUND, prev_state = VT_GROUND;

    /* Run state machine loop. */
    while (1) {
        /* Handle state exits. */
        if ((prev_state == VT_DCS_PASSTHROUGH) && (state != VT_DCS_PASSTHROUGH))
            cli_input_unhook(c);
        else if ((prev_state == VT_OSC_STRING) && (state != VT_OSC_STRING))
            cli_input_osc_end(c);
        prev_state = state;

        /* Read character. */
        c = getchar();
        cli_input_log_key("process", c);

        /* Interpret conditions for any state. */
        switch (c) {
            case 0x1b:
                /* Interpret ESC ESC as escaped ESC. Note that some terminals
                   may emit extended codes prefixed with ESC ESC, but there's
                   not much we can do to parse those. */
                if (state == VT_ESCAPE) {
                    cli_input_send(0x0001, 0);
                    state = VT_GROUND;
                } else {
                    state = VT_ESCAPE;
                }
                continue;

            case 0x7f:
                /* Ignore, unless this is an user-initiated Backspace. */
                if ((state == VT_GROUND) || (state == VT_ESCAPE))
                    break;
                else
                    continue;

            case EOF:
                /* Something went wrong. */
                cli_input_log("CLI Input: stdin read error\n");
                return;
        }

        /* Interpret conditions for specific states. */
        switch (state) {
            case VT_GROUND:
                switch (c) {
                    case 0x00 ... 0x1f:
                        cli_input_execute(c);
                        continue;

                    case 0x20 ... 0x7e: /* ASCII */
                        cli_input_send(ascii_seqs[c], 0);
                        break;

                    case 0x7f: /* Backspace */
                        cli_input_send(ascii_seqs['\b'], 0);
                        break;
                }
                break;

            case VT_ESCAPE:
                switch (c) {
                    case 0x00 ... 0x09:
                    case 0x0b ... 0x0c:
                    case 0x0e ... 0x1f:
                        cli_input_execute(c);
                        break;

                    case 0x0a: /* Esc Enter */
                    case 0x0d: /* Esc Enter (Windows) */
                        /* Block render thread. */
                        cli_render_monitorenter();

                        /* Disable raw input. */
                        cli_input_unraw();

                        /* Enter monitor loop. */
                        cli_monitor_thread(NULL);

                        /* Don't resume render thread if we're exiting. */
                        if (is_quit)
                            return;

                        /* Re-enable raw input. */
                        cli_input_raw();

                        /* Resume render thread. */
                        cli_render_monitorexit();

                        state = VT_GROUND;
                        break;

                    case 0x21 ... 0x2f:
                    case 0x4f:
                        cli_input_clear(c);
                        cli_input_collect(c);
                        state = VT_ESCAPE_INTERMEDIATE;
                        break;

                    case 0x20:
                    case 0x30 ... 0x4e:
                    case 0x51 ... 0x57:
                    case 0x59:
                    case 0x5a:
                    case 0x5c:
                    case 0x60 ... 0x7f:
                        cli_input_clear(c);
                        cli_input_esc_dispatch(c);
                        state = VT_GROUND;
                        break;

                    case 0x50:
                        state = VT_DCS_ENTRY;
                        cli_input_clear(c);
                        break;

                    case 0x58:
                    case 0x5e:
                    case 0x5f:
                        state = VT_SOS_PM_APC_STRING;
                        break;

                    case 0x5b:
                        state = VT_CSI_ENTRY;
                        cli_input_clear(c);
                        break;

                    case 0x5d:
                        state = VT_OSC_STRING;
                        cli_input_osc_start(c);
                        break;
                }
                break;

            case VT_ESCAPE_INTERMEDIATE:
                switch (c) {
                    case 0x00 ... 0x1a:
                    case 0x1c ... 0x1f:
                        cli_input_execute(c);
                        continue;

                    case 0x20 ... 0x2f:
                        cli_input_collect(c);
                        break;

                    case 0x30 ... 0x7e:
                        cli_input_esc_dispatch(c);
                        state = VT_GROUND;
                        break;
                }
                break;

            case VT_CSI_ENTRY:
                switch (c) {
                    case 0x00 ... 0x1a:
                    case 0x1c ... 0x1f:
                        cli_input_execute(c);
                        continue;

                    case 0x20 ... 0x2f:
                        cli_input_collect(c);
                        state = VT_ESCAPE_INTERMEDIATE;
                        break;

                    case 0x30 ... 0x39:
                    case 0x3b:
                        cli_input_param(c);
                        state = VT_CSI_PARAM;
                        break;

                    case 0x3a:
                        state = VT_CSI_IGNORE;
                        break;

                    case 0x3c ... 0x3f:
                        cli_input_collect(c);
                        state = VT_CSI_PARAM;
                        break;

                    case 0x40 ... 0x7e:
                        cli_input_csi_dispatch(c);
                        state = VT_GROUND;
                        break;
                }
                break;

            case VT_CSI_IGNORE:
                switch (c) {
                    case 0x00 ... 0x1a:
                    case 0x1c ... 0x1f:
                        cli_input_execute(c);
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
                        cli_input_execute(c);
                        continue;

                    case 0x20 ... 0x2f:
                        cli_input_collect(c);
                        state = VT_CSI_INTERMEDIATE;
                        break;

                    case 0x30 ... 0x39:
                    case 0x3b:
                        cli_input_param(c);
                        break;

                    case 0x3a:
                    case 0x3c ... 0x3f:
                        state = VT_CSI_IGNORE;
                        break;

                    case 0x40 ... 0x7e:
                        cli_input_csi_dispatch(c);
                        state = VT_GROUND;
                        break;
                }
                break;

            case VT_CSI_INTERMEDIATE:
                switch (c) {
                    case 0x00 ... 0x1a:
                    case 0x1c ... 0x1f:
                        cli_input_execute(c);
                        continue;

                    case 0x20 ... 0x2f:
                        cli_input_collect(c);
                        break;

                    case 0x30 ... 0x3f:
                        state = VT_CSI_IGNORE;
                        break;

                    case 0x40 ... 0x7e:
                        cli_input_csi_dispatch(c);
                        state = VT_GROUND;
                        break;
                }
                break;

            case VT_DCS_ENTRY:
                if (prev_state != VT_DCS_ENTRY)
                    cli_input_clear(c);
                switch (c) {
                    case 0x20 ... 0x2f:
                        cli_input_collect(c);
                        state = VT_DCS_INTERMEDIATE;
                        break;

                    case 0x30 ... 0x39:
                    case 0x3b:
                        cli_input_param(c);
                        state = VT_DCS_PARAM;
                        break;

                    case 0x3a:
                        state = VT_DCS_IGNORE;
                        break;

                    case 0x3c ... 0x3f:
                        cli_input_collect(c);
                        state = VT_DCS_PARAM;
                        break;

                    case 0x40 ... 0x7e:
                        state = VT_DCS_PASSTHROUGH;
                        cli_input_hook(c);
                        break;
                }
                break;

            case VT_DCS_INTERMEDIATE:
                switch (c) {
                    case 0x20 ... 0x2f:
                        cli_input_collect(c);
                        break;

                    case 0x30 ... 0x3f:
                        state = VT_DCS_IGNORE;
                        break;

                    case 0x40 ... 0x7e:
                        state = VT_DCS_PASSTHROUGH;
                        cli_input_hook(c);
                        break;
                }
                break;

            case VT_DCS_PARAM:
                switch (c) {
                    case 0x20 ... 0x2f:
                        cli_input_collect(c);
                        state = VT_DCS_INTERMEDIATE;
                        break;

                    case 0x30 ... 0x39:
                    case 0x3b:
                        cli_input_param(c);
                        break;

                    case 0x3a:
                    case 0x3c ... 0x3f:
                        state = VT_DCS_IGNORE;
                        break;

                    case 0x40 ... 0x7e:
                        state = VT_DCS_PASSTHROUGH;
                        cli_input_hook(c);
                        break;
                }
                break;

            case VT_DCS_PASSTHROUGH:
                switch (c) {
                    case 0x00 ... 0x7e:
                        cli_input_put(c);
                        break;
                }
                break;

            case VT_OSC_STRING:
                switch (c) {
                    case 0x20 ... 0x7e:
                        cli_input_osc_put(c);
                        break;
                }
                break;
        }
    }
}

void
cli_input_init()
{
    /* Don't initialize input altogether if stdin is not a tty. */
    if (!isatty(fileno(stdin))) {
        cli_input_log("CLI Input: stdin is not a tty\n");
        return;
    }
    cli_term.can_input = 1;

    /* Enable raw input. */
    cli_input_raw();

    /* Start input processing thread. */
    thread_create(cli_input_process, NULL);
}

void
cli_input_close()
{
    /* Restore terminal state. */
    cli_input_unraw();
}
