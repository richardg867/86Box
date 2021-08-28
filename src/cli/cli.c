/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Core module for the command line interface.
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
# include <windows.h>
# include <VersionHelpers.h>
#else
# include <ioctl.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/cli.h>


/* Lookup table for terminal types. */
static const struct {
    const char	  *name;
    const uint8_t color;
    const uint8_t ctl;
    const uint8_t gfx;
} term_types[] = {
#ifdef _WIN32
    {"\0cmd-nt6",	TERM_COLOR_4BIT,  0, 0}, /* prefix with NUL as these are invalid values */
    {"\0cmd-nt10",	TERM_COLOR_24BIT, 0, 0}, /* for TERM (not listed in terminfo database)  */
#endif
    {"iterm",		TERM_COLOR_24BIT, 0, 0},
    {"iterm2",		TERM_COLOR_24BIT, 0, TERM_GFX_PNG},
    {"kitty",		TERM_COLOR_24BIT, 0, TERM_GFX_PNG_KITTY}, /* not to be confused with the PuTTY fork */
    {"konsole",		TERM_COLOR_24BIT, 0, 0},
    {"linux",		TERM_COLOR_24BIT, 0, 0},
    {"mintty",		TERM_COLOR_24BIT, TERM_CTL_RAPIDBLINK | TERM_CTL_PRINT,	TERM_GFX_SIXEL | TERM_GFX_PNG},
    {"termite",		TERM_COLOR_24BIT, 0, 0}, /* not to be confused with the CompuPhase product */
    {"tmux",		TERM_COLOR_24BIT, 0, 0},
    {"vte",		TERM_COLOR_24BIT, 0, 0},
    {"xfce",		TERM_COLOR_24BIT, 0, 0},
    {"xterm-24bit",	TERM_COLOR_24BIT, 0, 0}, /* non-standard value not in terminfo database */
    {"xterm-24bits",	TERM_COLOR_24BIT, 0, 0}, /* same as above */
    {"putty",		TERM_COLOR_8BIT,  0, 0},
    {"xterm",		TERM_COLOR_8BIT,  0, 0}, /* DECRQSS query unlocks 24-bit if available */
    {"xterm-256color",	TERM_COLOR_8BIT,  0, 0}, /* same as above */
    {"xterm-16color",	TERM_COLOR_4BIT,  0, 0},
    {"vt100",		TERM_COLOR_NONE,  0, 0},
    {"vt220",		TERM_COLOR_NONE,  0, 0},
    {"vt240",		TERM_COLOR_NONE,  0, TERM_GFX_SIXEL},
    {NULL,		TERM_COLOR_3BIT,  0, 0} /* unknown terminal */
};

/* Entries for the CLI menu. */
static const char *menu_entries[] = {
    "[Enter] Go back to machine",
    "[R] Hard reset",
    "[Del] Send Ctrl+Alt+Del",
    "[E] Send Ctrl+Alt+Esc",
    "[S] Take screenshot",
    "[P] Pause",
    "[Q] Exit",
    NULL
};


cli_term_t	cli_term = {
    .size_x = 80, .size_y = 24, /* terminals default to 80x24, not the IBM PC's 80x25 */
    .setcolor = cli_render_setcolor_noop
};

static int	menu_entry_count = 0, menu_max_width = 0;

#define ENABLE_CLI_LOG 1
#ifdef ENABLE_CLI_LOG
int cli_do_log = ENABLE_CLI_LOG;

static void
cli_log(const char *fmt, ...)
{
    va_list ap;

    if (cli_do_log) {
	va_start(ap, fmt);
	pclog_ex(fmt, ap);
	va_end(ap);
    }
}
#else
#define cli_log(fmt, ...)
#endif


static int
cli_term_gettypeid(char *name) {
    /* Stop if the name is invalid. */
    if (!name || name[0] == '\0')
	return -1;

    /* Compare name with table entries. */
    for (int i = 0; term_types[i].name; i++) {
	if (!strcasecmp(name, term_types[i].name))
		return i;
    }

    /* No entry found. */
    return -1;
}


void
cli_term_setcolor(uint8_t level)
{
    cli_term.color_level = level;

    /* Tell the renderer that we have a new color level. */
    cli_render_setcolorlevel();
}


void
cli_term_setctl(uint8_t level)
{
    cli_term.ctl_level = level;
}


void
cli_term_setgfx(uint8_t level)
{
    cli_term.gfx_level = level;
}


void
cli_term_setsize(int size_x, int size_y, char *source)
{
     uint8_t new_size_x = MIN(size_x, 254), new_size_y = MIN(size_y, 254);

     cli_log("CLI: Terminal is %dx%d according to %s\n", new_size_x, new_size_y, source);

     if ((new_size_x != cli_term.size_x) || (new_size_y != cli_term.size_y)) {
 	cli_term.size_x = new_size_x;
 	cli_term.size_y = new_size_y;
     }
}


static void
cli_term_settype(int type)
{
    /* Use the final (NULL) definition on unknown terminal types. */
    if (type < 0)
	type = (sizeof(term_types) / sizeof(term_types[0])) - 1;

    /* Set feature levels for this terminal type definition. */
    cli_term_setcolor(term_types[type].color);
    cli_term_setctl(term_types[type].ctl);
    cli_term_setgfx(term_types[type].gfx);
}


void
cli_term_updatesize(int runtime)
{
    int sx = 0, sy = 0;

    /* Get terminal size through the OS. */
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h) {
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (GetConsoleScreenBufferInfo(h, &info)) {
		sx = info.srWindow.Right - info.srWindow.Left + 1;
		sy = info.srWindow.Bottom - info.srWindow.Top + 1;
	}

	/* While we're here on startup, enable ANSI output. */
	if (!runtime) {
		DWORD mode;
		if (GetConsoleMode(h, &mode)) {
			mode &= ~ENABLE_WRAP_AT_EOL_OUTPUT;
			mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			SetConsoleMode(h, mode);
		}
	}
    }
#else
    struct winsize sz;
    if (!ioctl(fileno(stdin), TIOCGWINSZ, &sz)) {
	sx = sz.ws_col;
	sy = sz.ws_row;
    }
#endif
    if ((sx > 1) && (sy > 1)) {
	cli_term_setsize(sx, sy, "OS");
	return;
    }

    /* Get terminal size through bash environment variables on startup. */
    char *env;
    if (!runtime &&
	(env = getenv("COLUMNS")) && (sscanf(env, "%d", &sx) == 1) && (sx > 1) &&
	(env = getenv("LINES")) && (sscanf(env, "%d", &sy) == 1) && (sy > 1))
	cli_term_setsize(sx, sy, "environment");

    /* Get terminal size through a CPR query, even if we already have
       bash environment variable data, since that may be inaccurate. */
    cli_term.cpr = 1;
    cli_render_write("\033[999;999H\033[6n\033[1;1H");
}


void
cli_init()
{
    /* Initialize input module. */
    cli_input_init();

    /* Initialize renderer module. */
    cli_render_init();

    /* Determine this terminal's type. */
    int id = cli_term_gettypeid(getenv("TERM_PROGRAM"));
    if (id == -1) {
	id = cli_term_gettypeid(getenv("TERM"));
#ifdef _WIN32
	/* Assume an unknown terminal on Windows to be cmd. */
	if (id == -1) {
		/* Account for cmd's more generous default sizes. */
		if (IsWindows10OrGreater()) {
			id = 1;
			cli_term.size_x = 100;
			cli_term.size_y = 30;
		} else {
			id = 0;
			cli_term.size_x = 80;
			cli_term.size_y = 25;
		}
	}
#endif
    }

#ifdef ENABLE_CLI_LOG
    if (id == -1) {
	cli_log("CLI: Unknown terminal type\n");
    } else {
	const char *term_name = term_types[id].name;
	if (!term_name[0])
		term_name++;
	cli_log("CLI: Detected terminal type: %s\n", term_name);
    }
#endif

    /* Set feature levels for this terminal. */
    cli_term_settype(id);

    /* Detect COLORTERM environment variable set by some 24-bit terminals. */
    if (cli_term.color_level < TERM_COLOR_24BIT) {
	char *value;
	if ((value = getenv("COLORTERM")) && (strcasecmp(value, "truecolor") || strcasecmp(value, "24bit"))) {
		cli_term_setcolor(TERM_COLOR_24BIT);
	} else {
		/* Start detecting the terminal's color capabilities through DECRQSS queries. */
		cli_term.decrqss_color = TERM_COLOR_24BIT;
		cli_render_write("\033[38;2;1;2;3m\033P$qm\033\\\033[0m");
	}
    }

    /* Determine the terminal's size. */
    cli_term_updatesize(0);

#ifdef SIGWINCH
    /* Redraw screen on terminal resize. */
    signal(SIGWINCH, cli_term_updatesize);
#endif

    /* Determine the longest menu entry. */
    for (menu_entry_count = 0; menu_entries[menu_entry_count]; menu_entry_count++) {
	int i = strlen(menu_entries[menu_entry_count]);
	if (i > menu_max_width)
		menu_max_width = i;
    }
}


void
cli_close()
{
    /* Stop the renderer module. */
    cli_render_close();
}
