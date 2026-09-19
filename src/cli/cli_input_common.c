/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Input translation module for the command line interface.
 *
 *
 *
 * Authors: RichardG, <richardg867@gmail.com>
 *
 *          Copyright 2026 RichardG.
 */

#include <stdint.h>
#include <stdarg.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/keyboard.h>
#include <86box/cli.h>

const uint16_t ascii_seqs[128] = {
    ['\b'] = 0x000e, /* terminals prefer 7F/del for backspace */
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
    [0x7f] = 0x000e
};

#define ENABLE_CLI_INPUT_COMMON_LOG 1
#ifdef ENABLE_CLI_INPUT_COMMON_LOG
int cli_input_common_do_log = ENABLE_CLI_INPUT_COMMON_LOG;

static void
cli_input_common_log(const char *fmt, ...)
{
    va_list ap;

    if (cli_input_common_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define cli_input_common_log(fmt, ...)
#endif

void
cli_input_send(uint16_t code, uint16_t modifier)
{
    cli_input_common_log("CLI Input: send(%04X, %03X)", code, modifier);

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
            modifier |= VT_SUPER;
            break;

        default:
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

        default:
            break;
    }

    /* Press key with modifiers unless an explicit key up with no explicit key down is requested. */
    if ((modifier & (VT_KEY_UP | VT_KEY_DOWN)) != VT_KEY_UP) {
        cli_input_common_log(" press");
        if (modifier & (VT_SUPER | VT_HYPER | VT_META))
            keyboard_input(1, 0xe05b);
        if (modifier & VT_CTRL)
            keyboard_input(1, 0x001d);
        if (modifier & VT_ALT)
            keyboard_input(1, 0x0038);
        if (modifier & VT_SHIFT)
            keyboard_input(1, 0x002a);
        if (modifier & VT_SHIFT_FAKE)
            keyboard_input(1, 0xe02a);
        if (code)
            keyboard_input(1, code);
    }

    /* Release key with modifiers if kitty event types are disabled or an explicit key up is requested. */
    if (
#ifdef USE_CLI
        !(cli_term.kitty_input & 2) ||
#endif
        (modifier & VT_KEY_UP)) {
        cli_input_common_log(" release");
        if (code)
            keyboard_input(0, code);
        if (modifier & VT_SHIFT_FAKE)
            keyboard_input(0, 0xe02a);
        if (modifier & VT_SHIFT)
            keyboard_input(0, 0x002a);
        if (modifier & VT_ALT)
            keyboard_input(0, 0x0038);
        if (modifier & VT_CTRL)
            keyboard_input(0, 0x001d);
        if (modifier & (VT_SUPER | VT_HYPER | VT_META))
            keyboard_input(0, 0xe05b);
    }

    cli_input_common_log("\n");
}
