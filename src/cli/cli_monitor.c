/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Monitor console module for the command line interface.
 *
 *
 *
 * Author:	Cacodemon345
 *		RichardG, <richardg867@gmail.com>
 *
 *		Copyright 2021 Cacodemon345.
 *		Copyright 2021 RichardG.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#ifdef _WIN32
# include <fcntl.h>
#else
# include <dlfcn.h>
# include <unistd.h>
#endif
#include <86box/86box.h>
#include <86box/version.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/cli.h>

#ifndef _WIN32
# ifdef __APPLE__
#  define LIBEDIT_LIBRARY "libedit.dylib"
# else
#  define LIBEDIT_LIBRARY "libedit.so"
# endif


extern int	fullscreen_pending; /* ugly hack */
#endif

static int	first_run = 1;
static event_t	*screenshot_event;

static char	*(*f_readline)(const char*) = NULL;
static int	(*f_add_history)(const char *) = NULL;
static void	(*f_rl_callback_handler_remove)(void) = NULL;


typedef struct {
    void	*func;
    int		ndrives;
    const char	*drive;
} media_cmd_t;


static int
cli_monitor_parsebool(char *arg)
{
    char ch = arg[0];
    if ((ch == 'o') || (ch == 'O')) {
	ch = arg[1];
	return (ch == 'n') || (ch == 'N');
    }
    return (ch == '1') || (ch == 'y') || (ch == 'Y') || (ch == 't') || (ch == 'T');
}


static int
cli_monitor_parsefile(char *path, int wp)
{
    if (strlen(path) >= PATH_MAX) {
	fprintf(CLI_RENDER_OUTPUT, "File path too long.\n");
	return -3;
    }

    FILE *f = fopen(path, "rb");
    if (f) {
	fclose(f);
	if (!wp) {
		f = fopen(path, "ab");
		if (f) {
			fclose(f);
		} else {
			if (errno == EPERM)
				fprintf(CLI_RENDER_OUTPUT, "No permission to write file, enabling write protection.\n");
			else
				fprintf(CLI_RENDER_OUTPUT, "File is read-only, enabling write protection.\n");	
			wp = 1;
		}
	}
	return wp;
    } else if (errno == EPERM) {
	fprintf(CLI_RENDER_OUTPUT, "No permission to read file: %s\n", path);
	return -2;
    } else {
	fprintf(CLI_RENDER_OUTPUT, "File not found: %s\n", path);
	return -1;
    }
}


static int
cli_monitor_parsemediaid(media_cmd_t *cmd, char *id_s)
{
    int id;
    if ((sscanf(id_s, "%d", &id) != 1) ||
	(id < 0) || (id >= cmd->ndrives)) {
	fprintf(CLI_RENDER_OUTPUT, "Invalid %s ID, expected 0-%d.\n", cmd->drive, cmd->ndrives);
	return -1;
    }
    return id;
}


static void
cli_monitor_mediaload(int argc, char **argv, const void *priv)
{
    /* Read media command information. */
    media_cmd_t *cmd = (media_cmd_t *) priv;
    void (*mount_func)(uint8_t id, char *fn, uint8_t wp) = cmd->func;

    /* Read and validate ID. */
    int id = cli_monitor_parsemediaid(cmd, argv[1]);
    if (id < 0)
	return;

    /* Read write protect flag. */
    int wp = (argc >= 3) && cli_monitor_parsebool(argv[3]);

    /* Validate file path. */
    wp = cli_monitor_parsefile(argv[2], wp);
    if (wp < 0)
	return;

    /* Provide feedback. */
    fprintf(CLI_RENDER_OUTPUT, "Inserting%simage into %s %d: %s\n",
	    (wp ? " write-protected " : " "),
	    cmd->drive, id,
	    argv[2]);

    /* Call mount function. */
    mount_func(id, argv[2], wp);
}


static void
cli_monitor_mediaload_nowp(int argc, char **argv, const void *priv)
{
    /* Read media command information. */
    media_cmd_t *cmd = (media_cmd_t *) priv;
    void (*mount_func)(uint8_t id, char *fn) = cmd->func;

    /* Read and validate ID. */
    int id = cli_monitor_parsemediaid(cmd, argv[1]);
    if (id < 0)
	return;

    /* Validate file path. */
    if (cli_monitor_parsefile(argv[2], 1) < 0)
	return;

    /* Provide feedback. */
    fprintf(CLI_RENDER_OUTPUT, "Inserting image into %s %d: %s\n",
	    cmd->drive, id, argv[2]);

    /* Call mount function. */
    mount_func(id, argv[2]);
}


static void
cli_monitor_mediaeject(int argc, char **argv, const void *priv)
{
    /* Read media command information. */
    media_cmd_t *cmd = (media_cmd_t *) priv;
    void (*eject_func)(uint8_t id) = cmd->func;

    /* Read and validate ID. */
    int id = cli_monitor_parsemediaid(cmd, argv[1]);
    if (id < 0)
	return;

    /* Provide feedback. */
    fprintf(CLI_RENDER_OUTPUT, "Ejecting image from %s %d.\n",
	    cmd->drive, id);

    /* Call eject function. */
    eject_func(id);
}


static void
cli_monitor_mediaeject_mountblank_nowp(int argc, char **argv, const void *priv)
{
    /* Read media command information. */
    media_cmd_t *cmd = (media_cmd_t *) priv;
    void (*mount_func)(uint8_t id, char *fn) = cmd->func;

    /* Read and validate ID. */
    int id = cli_monitor_parsemediaid(cmd, argv[1]);
    if (id < 0)
	return;

    /* Provide feedback. */
    fprintf(CLI_RENDER_OUTPUT, "Ejecting image from %s %d.\n",
	    cmd->drive, id);

    /* Call eject function. */
    mount_func(id, "");
}


static void	cli_monitor_help(int argc, char **argv, const void *priv);


static void
cli_monitor_hardreset(int argc, char **argv, const void *priv)
{
    fprintf(CLI_RENDER_OUTPUT, "Hard resetting emulated machine.\n");
    pc_reset_hard();
}


static void
cli_monitor_pause(int argc, char **argv, const void *priv)
{
    plat_pause(dopause ^ 1);
    fprintf(CLI_RENDER_OUTPUT, "Emulated machine %saused.\n", dopause ? "p" : "unp");
}


static void
cli_monitor_fullscreen(int argc, char **argv, const void *priv)
{
    video_fullscreen ^= 1;
#ifndef _WIN32
    fullscreen_pending = 1;
#endif
    fprintf(CLI_RENDER_OUTPUT, "Fullscreen mode %s.\n", video_fullscreen ? "entered" : "exited");
}


static void
cli_monitor_screenshot_hook(char *path)
{
    /* This hook should only be called once. */
    screenshot_hook = NULL;

    /* Print screenshot path. */
    fprintf(CLI_RENDER_OUTPUT, "Saved screenshot to: %s\n", path);

#ifdef USE_CLI
    /* Output screenshot if supported by the terminal. */
    if (cli_term.gfx_level & (TERM_GFX_PNG | TERM_GFX_PNG_KITTY)) {
	FILE *f = fopen(path, "rb");
	if (f) {
		int read;
		uint8_t buf[3072];

		/* Render PNG. */
		if (cli_term.gfx_level & TERM_GFX_PNG) {
			/* Output header. */
			fputs("\033]1337;File=name=", CLI_RENDER_OUTPUT);
			path = plat_get_basename((const char *) path);
			cli_render_process_base64((uint8_t *) path, strlen(path));
			fseek(f, 0, SEEK_END);
			fprintf(CLI_RENDER_OUTPUT, ";size=%ld:", ftell(f));
			fseek(f, 0, SEEK_SET);

			/* Output image. */
			while ((read = fread(&buf, 1, sizeof(buf), f)))
				cli_render_process_base64(buf, read);

			/* Output terminator. */
			fputc('\a', CLI_RENDER_OUTPUT);
		} else if (cli_term.gfx_level & TERM_GFX_PNG_KITTY) {
			/* Output image in chunks of up to 4096
			   base64-encoded bytes (3072 real bytes). */
			int i = 1;
			while ((read = fread(&buf, 1, sizeof(buf), f))) {
				/* Output header. */
				fputs("\033_G", CLI_RENDER_OUTPUT);
				if (i) {
					i = 0;
					fputs("a=T,f=100,q=1,", CLI_RENDER_OUTPUT);
				}
				fprintf(CLI_RENDER_OUTPUT, "m=%d;", !feof(f));

				/* Output chunk data as base64. */
				cli_render_process_base64(buf, read);

				/* Output terminator. */
				fputs("\033\\", CLI_RENDER_OUTPUT);
			}
		}

		/* Finish and flush output. */
		fputc('\n', CLI_RENDER_OUTPUT);
		fflush(CLI_RENDER_OUTPUT);

		fclose(f);
	}
    } else if (cli_term.gfx_level & TERM_GFX_SIXEL) {
	cli_render_process_sixel_png(path);
    }
#endif

    /* Allow monitor thread to proceed. */
    thread_set_event(screenshot_event);
}


static void
cli_monitor_screenshot(int argc, char **argv, const void *priv)
{
    /* Set up screenshot hook. */
    screenshot_event = thread_create_event();
    screenshot_hook = cli_monitor_screenshot_hook;

    /* Take screenshot. */
#ifdef _WIN32 /* temporary, while unix take_screenshot is not implemented */
    take_screenshot();
#else
    startblit();
    screenshots++;
    endblit();
# include <86box/device.h>
    device_force_redraw();
#endif

    /* Wait for the hook. */
    thread_wait_event(screenshot_event, -1);
    thread_destroy_event(screenshot_event);
}


static void
cli_monitor_exit(int argc, char **argv, const void *priv)
{
    fprintf(CLI_RENDER_OUTPUT, "Exiting.\n");
    do_stop();
}


enum {
    MONITOR_CATEGORY_MEDIALOAD = 0,
    MONITOR_CATEGORY_MEDIAEJECT,
    MONITOR_CATEGORY_EMULATOR
};

static const struct {
    const char	*name, *helptext, **args;
    int		args_min, args_max, exit, category;
    void	(*handler)(int argc, char **argv, const void *priv);
    const void	*priv;
} commands[] = {
    {
	.name = "fddload",
	.helptext = "Load floppy disk image into drive <id>.",
	.args = (const char*[]) { "id", "filename", "writeprotect" },
	.args_min = 2, .args_max = 3,
	.category = MONITOR_CATEGORY_MEDIALOAD,
	.handler = cli_monitor_mediaload,
	.priv = (const media_cmd_t[]) { [0] = { floppy_mount, 4, "floppy drive" } }
    }, {
	.name = "cdload",
	.helptext = "Load CD-ROM image into drive <id>.",
	.args = (const char*[]) { "id", "filename" },
	.args_min = 2, .args_max = 2,
	.category = MONITOR_CATEGORY_MEDIALOAD,
	.handler = cli_monitor_mediaload_nowp,
	.priv = (const media_cmd_t[]) { [0] = { cdrom_mount, 4, "CD-ROM drive" } }
    }, {
	.name = "zipload",
	.helptext = "Load ZIP disk image into drive <id>.",
	.args = (const char*[]) { "id", "filename", "writeprotect" },
	.args_min = 2, .args_max = 3,
	.category = MONITOR_CATEGORY_MEDIALOAD,
	.handler = cli_monitor_mediaload,
	.priv = (const media_cmd_t[]) { [0] = { zip_mount, 4, "ZIP drive" } }
    }, {
	.name = "moload",
	.helptext = "Load MO disk image into drive <id>.",
	.args = (const char*[]) { "id", "filename", "writeprotect" },
	.args_min = 2, .args_max = 3,
	.category = MONITOR_CATEGORY_MEDIALOAD,
	.handler = cli_monitor_mediaload,
	.priv = (const media_cmd_t[]) { [0] = { mo_mount, 4, "MO drive" } }
    }, {
	.name = "cartload",
	.helptext = "Load cartridge image into slot <id>.",
	.args = (const char*[]) { "id", "filename", "writeprotect" },
	.args_min = 2, .args_max = 3,
	.category = MONITOR_CATEGORY_MEDIALOAD,
	.handler = cli_monitor_mediaload,
	.priv = (const media_cmd_t[]) { [0] = { cartridge_mount, 2, "cartridge slot" } }
    },

    {
	.name = "fddeject",
	.helptext = "Eject disk from floppy drive <id>.",
	.args = (const char*[]) { "id" },
	.args_min = 1, .args_max = 1,
	.category = MONITOR_CATEGORY_MEDIAEJECT,
	.handler = cli_monitor_mediaeject,
	.priv = (const media_cmd_t[]) { [0] = { floppy_eject, 4, "floppy drive" } }
    }, {
	.name = "cdeject",
	.helptext = "Eject disc from CD-ROM drive <id>.",
	.args = (const char*[]) { "id" },
	.args_min = 1, .args_max = 1,
	.category = MONITOR_CATEGORY_MEDIAEJECT,
	.handler = cli_monitor_mediaeject_mountblank_nowp,
	.priv = (const media_cmd_t[]) { [0] = { cdrom_mount, 4, "CD-ROM drive" } }
    }, {
	.name = "zipeject",
	.helptext = "Eject disk from ZIP drive <id>.",
	.args = (const char*[]) { "id" },
	.args_min = 1, .args_max = 1,
	.category = MONITOR_CATEGORY_MEDIAEJECT,
	.handler = cli_monitor_mediaeject,
	.priv = (const media_cmd_t[]) { [0] = { zip_eject, 4, "ZIP drive" } }
    }, {
	.name = "moeject",
	.helptext = "Eject disk from MO drive <id>.",
	.args = (const char*[]) { "id" },
	.args_min = 1, .args_max = 1,
	.category = MONITOR_CATEGORY_MEDIAEJECT,
	.handler = cli_monitor_mediaeject,
	.priv = (const media_cmd_t[]) { [0] = { mo_eject, 4, "MO drive" } }
    }, {
	.name = "carteject",
	.helptext = "Eject cartridge from slot <id>.",
	.args = (const char*[]) { "id" },
	.args_min = 1, .args_max = 1,
	.category = MONITOR_CATEGORY_MEDIAEJECT,
	.handler = cli_monitor_mediaeject,
	.priv = (const media_cmd_t[]) { [0] = { cartridge_eject, 2, "cartridge slot" } }
    },

    {
	.name = "hardreset",
	.helptext = "Hard reset the emulated machine.",
	.category = MONITOR_CATEGORY_EMULATOR,
	.handler = cli_monitor_hardreset
    }, {
	.name = "pause",
	.helptext = "Pause or unpause the emulated machine.",
	.category = MONITOR_CATEGORY_EMULATOR,
	.handler = cli_monitor_pause
    }, {
	.name = "fullscreen",
	.helptext = "Enter or exit fullscreen mode.",
	.category = MONITOR_CATEGORY_EMULATOR,
	.handler = cli_monitor_fullscreen
    }, {
	.name = "screenshot",
	.helptext = "Take a screenshot.",
	.category = MONITOR_CATEGORY_EMULATOR,
	.handler = cli_monitor_screenshot
    }, {
	.name = "exit",
	.helptext = "Exit " EMU_NAME ".",
	.exit = 1,
	.category = MONITOR_CATEGORY_EMULATOR,
	.handler = cli_monitor_exit
#ifdef USE_CLI
    }, {
	.name = "back",
	.helptext = "Return to the screen.",
	.exit = 1,
	.category = MONITOR_CATEGORY_EMULATOR
#endif
    }, {
	.name = "help",
	.args_max = 1,
	.category = MONITOR_CATEGORY_EMULATOR,
	.handler = cli_monitor_help
    }, {0}
};


static void
cli_monitor_printargs(int cmd)
{
    /* Make sure this is a valid command. */
    if ((cmd >= (sizeof(commands) / sizeof(commands[0]))) ||
	!commands[cmd].name)
	return;

    /* Print command name. */
    fputs(commands[cmd].name, CLI_RENDER_OUTPUT);

    /* Print command arguments if applicable. */
    if (!commands[cmd].args)
	return;
    for (int arg = 0; arg < commands[cmd].args_max; arg++) {
	if (arg < commands[cmd].args_min)
		fprintf(CLI_RENDER_OUTPUT, " <%s>", commands[cmd].args[arg]);
	else
		fprintf(CLI_RENDER_OUTPUT, " [%s]", commands[cmd].args[arg]);
    }
}


static void
cli_monitor_usage(int cmd)
{
    cli_monitor_printargs(cmd);
    fprintf(CLI_RENDER_OUTPUT, "\n");
    if (commands[cmd].helptext)
	fprintf(CLI_RENDER_OUTPUT, "- %s\n", commands[cmd].helptext);
}


static void
cli_monitor_help(int argc, char **argv, const void *priv)
{
    /* Print help for a specific command if one was provided. */
    int cmd;
    if (argc) {
	for (cmd = 0; commands[cmd].name; cmd++) {
		if (strcasecmp(commands[cmd].name, argv[1]))
			continue;
		cli_monitor_usage(cmd);
		return;
	}
	fprintf(CLI_RENDER_OUTPUT, "Command not found.\n");
	return;
    }

    /* List all commands. */
    int category = 0;
    for (cmd = 0; commands[cmd].name; cmd++) {
	/* Don't list commands with no helptext. */
	if (!commands[cmd].helptext)
		continue;

	/* Print blank line if this is a new category. */
	if (commands[cmd].category != category) {
		category = commands[cmd].category;
		fprintf(CLI_RENDER_OUTPUT, "\n");
	}

	/* Print arguments and helptext. */
	cli_monitor_printargs(cmd);
	fprintf(CLI_RENDER_OUTPUT, " - %s\n", commands[cmd].helptext);
    }
}


void
cli_monitor_thread(void *priv)
{
    if (!isatty(fileno(stdin)) || !isatty(fileno(CLI_RENDER_OUTPUT)))
	return;

    if (first_run) {
	first_run = 0;
	fprintf(CLI_RENDER_OUTPUT, EMU_NAME " monitor console.\n");
    }

    char buf[4096], *line = NULL, *line_copy, *argv[8],
	 ch, in_quote;
    int argc, end, i, j, arg_start;

    /* Read and process commands. */
    while (!feof(stdin)) {
	/* Read line. */
	if (f_readline) {
		line = f_readline("(" EMU_NAME ") ");
	} else {
		fprintf(CLI_RENDER_OUTPUT, "(" EMU_NAME ") ");
		line = fgets(buf, sizeof(buf), stdin);
	}
	if (!line)
		continue;

	/* Remove trailing newline characters. */
	end = strlen(line) - 1;
	while (end && (((line[end] == '\r') || (line[end] == '\n'))))
		line[end--] = '\0';

	/* Add line to libedit history. */
	if (f_add_history)
		f_add_history(line);

	/* Allocate new line buffer. */
	line_copy = malloc(end + 2);
	if (!line_copy)
		goto next;

	/* Parse arguments. */
	memset(argv, 0, sizeof(argv));
	argc = arg_start = 0;
	in_quote = 0;
	for (i = j = 0; i <= end; i++) {
		ch = line[i];
		if (ch == '\\') {
#ifdef _WIN32		/* On Windows, treat \ as a path separator if the
			   next character is a valid filename character. */
			ch = line[i + 1];
			if ((ch != '\\') && (in_quote || (ch != ' ')) &&
			    (ch != '/') && (ch != ':') && (ch != '*') && (ch != '?') &&
			    (ch != '"') && (ch != '<') && (ch != '>') && (ch != '|')) {
				line_copy[j++] = '\\';
				continue;
			}
#endif
			/* Add escaped character. */
			line_copy[j++] = line[++i];
		} else if ((ch == '"') || (ch == '\'')) {
			/* Enter or exit quote mode. */
			if (!in_quote)
				in_quote = ch;
			else if (in_quote == ch)
				in_quote = 0;
			else
				line_copy[j++] = ch;
		} else if (!in_quote && (line[i] == ' ')) {
			/* Terminate and save this argument. */
			line_copy[j++] = '\0';
			argv[argc++] = &line_copy[arg_start];
			arg_start = j;

			/* Stop if we have too many arguments. */
			if (argc >= (sizeof(argv) / sizeof(argv[0])))
				goto have_args;
		} else {
			/* Add character. */
			line_copy[j++] = ch;
		}
	}
	/* Add final argument. */
	line_copy[j++] = '\0';
	argv[argc++] = &line_copy[arg_start];

have_args:
	argc--;
	if (f_readline)
		free(line);

	/* Go through the command list. */
	for (i = 0; commands[i].name; i++) {
		/* Move on if this is not the command we're looking for. */
		if (strcasecmp(argv[0], commands[i].name))
			continue;

		/* Check number of arguments. */
		if ((argc < commands[i].args_min) || (argc > commands[i].args_max)) {
			/* Print usage and don't process this command. */
			cli_monitor_usage(i);
			break;
		}

		/* Call command handler. */
		if (commands[i].handler)
			commands[i].handler(argc, argv, commands[i].priv);

		/* Stop thread if this is an exiting command. */
		if (commands[i].exit)
			return;

		break;
	}

next:	if (line_copy)
		free(line_copy);
	line = NULL;
    }
}


void
cli_monitor_init(uint8_t independent)
{
    if (!isatty(fileno(stdin)) || !isatty(fileno(CLI_RENDER_OUTPUT)))
	return;

#ifndef _WIN32
    void *libedithandle = dlopen(LIBEDIT_LIBRARY, RTLD_LOCAL | RTLD_LAZY);
    if (libedithandle) {
	f_readline = dlsym(libedithandle, "readline");
	f_add_history = dlsym(libedithandle, "add_history");
	if (!f_readline)
		fprintf(CLI_RENDER_OUTPUT, "readline in libedit not found, monitor line editing will be limited.\n");
	f_rl_callback_handler_remove = dlsym(libedithandle, "rl_callback_handler_remove");
	FILE **f_rl_outstream = dlsym(libedithandle, "rl_outstream");
	if (f_rl_outstream)
		*f_rl_outstream = CLI_RENDER_OUTPUT;
    } else {
	fprintf(CLI_RENDER_OUTPUT, "libedit not found, monitor line editing will be limited.\n");
    }
#endif

    if (independent) {
	/* Start monitor processing thread. */
	thread_create(cli_monitor_thread, NULL);
    }
}


void
cli_monitor_close()
{
    if (f_rl_callback_handler_remove)
	f_rl_callback_handler_remove();
}
