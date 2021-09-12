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
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/cli.h>

#ifndef _WIN32
# ifdef __APPLE__
#  define LIBEDIT_LIBRARY "libedit.dylib"
# else
#  define LIBEDIT_LIBRARY "libedit.so"
# endif


extern int	fullscreen_pending; /* ugly hack */
#endif

static char	*xargv[512];

static char	*(*f_readline)(const char*) = NULL;
static int	(*f_add_history)(const char *) = NULL;
static void	(*f_rl_callback_handler_remove)(void) = NULL;


/* From musl. */
static char *
local_strsep(char **str, const char *sep)
{
    char *s = *str, *end;
    if (!s)
	return NULL;

    end = s + strcspn(s, sep);
    if (*end)
	*end++ = 0;
    else
	end = 0;
    *str = end;

    return s;
}


static uint8_t
process_media_commands_3(uint8_t *id, char *fn, uint8_t *wp, int cmdargc)
{
    uint8_t err = 0;
    *id = atoi(xargv[1]);

    if (xargv[2][0] == '\'' || xargv[2][0] == '"') {
	int curarg = 2;
	for (curarg = 2; curarg < cmdargc; curarg++) {
		if (strlen(fn) + strlen(xargv[curarg]) >= PATH_MAX) {
			err = 1;
			fprintf(CLI_RENDER_OUTPUT, "Path name too long.\n");
		}

		strcat(fn, xargv[curarg] + (xargv[curarg][0] == '\'' || xargv[curarg][0] == '"'));
		if (fn[strlen(fn) - 1] == '\'' ||
			fn[strlen(fn) - 1] == '"') {
			if (curarg + 1 < cmdargc)
				*wp = atoi(xargv[curarg + 1]);
			break;
		}
		strcat(fn, " ");
	}
    } else {
	if (strlen(xargv[2]) < PATH_MAX) {
		strcpy(fn, xargv[2]);
		*wp = atoi(xargv[3]);
	} else {
		fprintf(CLI_RENDER_OUTPUT, "Path name too long.\n");
		err = 1;
	}
    }

    if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
	fn[strlen(fn) - 1] = '\0';

    return err;
}


void
cli_monitor_thread(void *priv)
{
    if (!isatty(fileno(stdin)) || !isatty(fileno(stdout)))
	return;

    char *line = NULL, buf[4096];

    fprintf(CLI_RENDER_OUTPUT, "86Box monitor console.\n");
    while (!feof(stdin)) {
	if (f_readline) {
		line = f_readline("(86Box) ");
	} else {
		fprintf(CLI_RENDER_OUTPUT, "(86Box) ");
		line = fgets(buf, sizeof(buf), stdin);
	}
	if (!line)
		continue;

	int cmdargc = 0;
	char* linecpy;
	line[strcspn(line, "\r\n")] = '\0';
	linecpy = strdup(line);
	if (!linecpy) {
		free(line);
		line = NULL;
		continue;
	}

	if (f_add_history)
		f_add_history(line);

	memset(xargv, 0, sizeof(xargv));
	while (1) {
		xargv[cmdargc++] = local_strsep(&linecpy, " ");
		if (xargv[cmdargc - 1] == NULL || cmdargc >= 512)
			break;
	}
	cmdargc--;

	if (strncasecmp(xargv[0], "help", 4) == 0) {
		printf("fddload <id> <filename> <wp> - Load floppy disk image into drive <id>.\n"
		       "cdload <id> <filename> - Load CD-ROM image into drive <id>.\n"
		       "zipload <id> <filename> <wp> - Load ZIP image into ZIP drive <id>.\n"
		       "cartload <id> <filename> <wp> - Load cartridge image into cartridge drive <id>.\n"
		       "moload <id> <filename> <wp> - Load MO image into MO drive <id>.\n"
		       "\n"
		       "fddeject <id> - eject disk from floppy drive <id>.\n"
		       "cdeject <id> - eject disc from CD-ROM drive <id>.\n"
		       "zipeject <id> - eject ZIP image from ZIP drive <id>.\n"
		       "carteject <id> - eject cartridge from drive <id>.\n"
		       "moeject <id> - eject image from MO drive <id>.\n"
		       "\n"
		       "hardreset - hard reset the emulated system.\n"
		       "pause - pause the the emulated system.\n"
		       "fullscreen - toggle fullscreen.\n"
		       "exit - exit 86Box.\n"
#ifdef USE_CLI
		       "back - Return to the screen.\n"
#endif
		       );
#ifdef USE_CLI
	} else if (strncasecmp(xargv[0], "back", 4) == 0) {
		break;
#endif
	} else if (strncasecmp(xargv[0], "exit", 4) == 0) {
		do_stop();
		break;
	} else if (strncasecmp(xargv[0], "fullscreen", 10) == 0) {
		video_fullscreen = 1;
#ifndef _WIN32
		fullscreen_pending = 1;
#endif
	}
	else if (strncasecmp(xargv[0], "pause", 5) == 0) {
		plat_pause(dopause ^ 1);
		fprintf(CLI_RENDER_OUTPUT, "%s", dopause ? "Paused.\n" : "Unpaused.\n");
	} else if (strncasecmp(xargv[0], "hardreset", 9) == 0) {
		pc_reset_hard();
	} else if ((strncasecmp(xargv[0], "cdload", 6) == 0) && (cmdargc >= 3)) {
		uint8_t id, err = 0;
		char fn[PATH_MAX];

		if (!xargv[2] || !xargv[1]) {
			free(line);
			free(linecpy);
			line = NULL;
			continue;
		}

		id = atoi(xargv[1]);
		memset(fn, 0, sizeof(fn));
		if (xargv[2][0] == '\'' || xargv[2][0] == '"') {
			int curarg = 2;
			for (; curarg < cmdargc; curarg++) {
				if (strlen(fn) + strlen(xargv[curarg]) >= PATH_MAX) {
					err = 1;
					fprintf(CLI_RENDER_OUTPUT, "Path name too long.\n");
				}
			}
			strcat(fn, xargv[curarg] + (xargv[curarg][0] == '\'' || xargv[curarg][0] == '"'));

			if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
				break;

			strcat(fn, " ");
		} else {
			if (strlen(xargv[2]) < PATH_MAX)
				strcpy(fn, xargv[2]);
			else
				fprintf(CLI_RENDER_OUTPUT, "Path name too long.\n");
		}
		if (!err) {
			if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
				fn[strlen(fn) - 1] = '\0';
			fprintf(CLI_RENDER_OUTPUT, "Inserting disc into CD-ROM drive %d: %s\n", id, fn);
			cdrom_mount(id, fn);
		}
	} else if ((strncasecmp(xargv[0], "fddeject", 8) == 0) && (cmdargc >= 2)) {
		floppy_eject(atoi(xargv[1]));
	} else if ((strncasecmp(xargv[0], "cdeject", 8) == 0) && (cmdargc >= 2)) {
		cdrom_mount(atoi(xargv[1]), "");
	} else if ((strncasecmp(xargv[0], "moeject", 8) == 0) && (cmdargc >= 2)) {
		mo_eject(atoi(xargv[1]));
	} else if ((strncasecmp(xargv[0], "carteject", 8) == 0) && (cmdargc >= 2)) {
		cartridge_eject(atoi(xargv[1]));
	} else if ((strncasecmp(xargv[0], "zipeject", 8) == 0) && (cmdargc >= 2)) {
		zip_eject(atoi(xargv[1]));
	} else if ((strncasecmp(xargv[0], "fddload", 7) == 0) && (cmdargc >= 4)) {
		uint8_t id, wp;
		char fn[PATH_MAX];
		memset(fn, 0, sizeof(fn));
		if (!xargv[2] || !xargv[1]) {
			free(line);
			free(linecpy);
			line = NULL;
			continue;
		}

		if (!process_media_commands_3(&id, fn, &wp, cmdargc)) {
			if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
				fn[strlen(fn) - 1] = '\0';
			fprintf(CLI_RENDER_OUTPUT, "Inserting disk into floppy drive %c: %s\n", id + 'A', fn);
			floppy_mount(id, fn, wp);
		}
	} else if ((strncasecmp(xargv[0], "moload", 7) == 0) && (cmdargc >= 4)) {
		uint8_t id, wp;
		char fn[PATH_MAX];
		memset(fn, 0, sizeof(fn));
		if (!xargv[2] || !xargv[1]) {
			free(line);
			free(linecpy);
			line = NULL;
			continue;
		}

		if (!process_media_commands_3(&id, fn, &wp, cmdargc)) {
			if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
				fn[strlen(fn) - 1] = '\0';
			fprintf(CLI_RENDER_OUTPUT, "Inserting into mo drive %d: %s\n", id, fn);
			mo_mount(id, fn, wp);
		}
	} else if (strncasecmp(xargv[0], "cartload", 7) == 0 && cmdargc >= 4) {
		uint8_t id, wp;
		char fn[PATH_MAX];
		memset(fn, 0, sizeof(fn));
		if (!xargv[2] || !xargv[1]) {
			free(line);
			free(linecpy);
			line = NULL;
			continue;
		}

		if (!process_media_commands_3(&id, fn, &wp, cmdargc)) {
			if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
				fn[strlen(fn) - 1] = '\0';
			fprintf(CLI_RENDER_OUTPUT, "Inserting tape into cartridge holder %d: %s\n", id, fn);
			cartridge_mount(id, fn, wp);
		}
	} else if ((strncasecmp(xargv[0], "zipload", 7) == 0) && (cmdargc >= 4)) {
		uint8_t id, wp;
		char fn[PATH_MAX];
		memset(fn, 0, sizeof(fn));
		if (!xargv[2] || !xargv[1]) {
			free(line);
			free(linecpy);
			line = NULL;
			continue;
		}

		if (!process_media_commands_3(&id, fn, &wp, cmdargc)) {
			if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
				fn[strlen(fn) - 1] = '\0';
			fprintf(CLI_RENDER_OUTPUT, "Inserting disk into ZIP drive %c: %s\n", id + 'A', fn);
			zip_mount(id, fn, wp);
		}
	}

	if (f_readline)
		free(line);
	free(linecpy);
	line = NULL;
    }
}


void
cli_monitor_init(uint8_t independent)
{
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
