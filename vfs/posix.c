/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The C library's file and process calls on the node table: open() resolves
 * bare node names and paths and opens files of mounted file systems, stdout
 * is line buffered, and system() runs commands by name or path (`reboot` is
 * built in) and takes `<`, `>` and `>>` off their command lines.
 *
 * Handles 0, 1 and 2 go through the running task's streams (kernel/task.h), so
 * every task can have its own; one that has none follows the console.
 * All others are entries of the table in vfs.c.
 */

#include "vfs.h"
#include "../bin/tmp.h"
#include "../kernel/task.h"
#include "../lib/args.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------- */

#define POSIX_STDOUT_SIZE 128u /* stdout line buffer */

static char posix_stdout_buf[POSIX_STDOUT_SIZE];
static unsigned int posix_stdout_pos = 0;
static int posix_stdout_fd = -1; /* where the buffered line goes */

int vfs_stream(int handle)
{
	if (handle < 0 || handle > 2) {
		return handle;
	}

	task_streams_t *s = task_streams();
	int fd = (handle == 0) ? s->in : s->out;

	return (fd >= 0) ? fd : vfs_console();
}

void vfs_stdout_flush(void)
{
	unsigned int size = posix_stdout_pos;

	posix_stdout_pos = 0;

	if (size > 0) {
		vfs_fd_write(posix_stdout_fd, posix_stdout_buf, size);
	}
}

/* stdout is line buffered; a line of another task's stdout goes out first */
static int posix_stdout_write(int fd, const char *buffer, unsigned int len)
{
	bool crlf = vfs_terminal(); /* a terminal needs "\r\n" to start a line */

	if (fd != posix_stdout_fd) {
		vfs_stdout_flush();
		posix_stdout_fd = fd;
	}

	for (unsigned int i = 0; i < len; i++) {
		if (buffer[i] == '\n' && crlf) {
			posix_stdout_buf[posix_stdout_pos++] = '\r';
		}

		posix_stdout_buf[posix_stdout_pos++] = buffer[i];

		/* pass the line on at '\n', or while room for "\r\n" remains */
		if (buffer[i] != '\n' && posix_stdout_pos < POSIX_STDOUT_SIZE - 1) {
			continue;
		}

		int size = posix_stdout_pos;
		posix_stdout_pos = 0;

		if (vfs_fd_write(fd, posix_stdout_buf, size) < 0) {
			return -1;
		}
	}

	return len;
}

/* -------------------------------------------------------------------------- */

/*
 * Returns a handle on the node or file at `pathname`. O_CREAT in a table
 * directory creates a RAM file, as `touch`.
 */
int __attribute__((__section__(".libc.open"))) open(const char *pathname, int flags, int mode)
{
	vfs_path_t p;

	if (vfs_resolve(pathname, &p) < 0) {
		return -1;
	}

	if (p.kind == VFS_NODE) {
		if (flags & O_TRUNC) {
			tmp_file_truncate(vfs_nodes_get() + p.node);
		}

		return vfs_node_open(p.node, flags);
	}

	if (p.kind == VFS_FILE || (p.kind == VFS_NONE && vfs_mount_get(p.dir, NULL) != NULL)) {
		return vfs_file_open(&p, flags);
	}

	if (p.kind == VFS_NONE && (flags & O_CREAT)) {
		int node = vfs_tmp_create(&p);

		return (node < 0) ? -1 : vfs_node_open(node, flags);
	}

	return -1;
}

int __attribute__((__section__(".libc.close"))) close(int handle)
{
	/* what stdout still holds for it goes first */
	if (handle == posix_stdout_fd) {
		vfs_stdout_flush();
	}

	return vfs_fd_close(handle);
}

int __attribute__((__section__(".libc.write"))) write(int handle, void *buffer, unsigned int len)
{
	int fd = vfs_stream(handle);

	if (handle != 1) {
		return vfs_fd_write(fd, buffer, len);
	}

	if (buffer == NULL || fd < 0) {
		return -1;
	}

	return posix_stdout_write(fd, buffer, len);
}

int __attribute__((__section__(".libc.read"))) read(int handle, void *buffer, unsigned int len)
{
	return vfs_fd_read(vfs_stream(handle), buffer, len);
}

/* files: the full 32-bit position (a node's lseek handler returns an int) */
off_t __attribute__((__section__(".libc.lseek"))) lseek(int handle, off_t offset, int whence)
{
	return (off_t)vfs_fd_seek(handle, (int32_t)offset, whence);
}

/* -------------------------------------------------------------------------- */

#define POSIX_SYSTEM_SIZE 128u /* longest command line */
#define POSIX_ARGS_MAX	  16   /* most words in a command line */

static char posix_system_string[POSIX_SYSTEM_SIZE];

/* a new task holds what it inherited, until vfs_streams_close() */
void vfs_streams_hold(int in, int out)
{
	vfs_fd_hold(in);
	vfs_fd_hold(out);
}

void vfs_streams_close(int in, int out)
{
	if (in >= 0) {
		close(in);
	}

	if (out >= 0) {
		close(out);
	}
}

#ifndef VFS_NO_REDIRECT
/*
 * `< path`, `> path` and `>> path` of a command line, as in a Linux shell: they
 * can stand anywhere, need no blanks, and the last of a kind counts. Each is
 * opened and blanked out of @p line, so the command never sees it; quoted text
 * is left alone. Sets @p s; `>` ends a terminal, as a file wants plain "\n".
 * Returns 0, or -1 and leaves what it opened in @p s.
 */
static int posix_redirect(char *line, task_streams_t *s, const task_streams_t *saved)
{
	bool quoted = false;

	for (char *p = line; *p != '\0'; p++) {
		if (*p == '"') {
			quoted = !quoted;
		}

		if (quoted || (*p != '<' && *p != '>')) {
			continue;
		}

		char *start = p;
		bool input = (*p++ == '<');
		int flags = input ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);

		if (!input && *p == '>') {
			flags = O_WRONLY | O_CREAT | O_APPEND;
			p++;
		}

		while (*p == ' ' || *p == '\t') {
			p++;
		}

		bool quote = (*p == '"');

		p += quote ? 1 : 0;

		char *path = p;

		while (*p != '\0' && (quote ? *p != '"' : strchr(" \t<>", *p) == NULL)) {
			p++;
		}

		char end = *p;

		*p = '\0';

		int fd = (p > path) ? open(path, flags, 0) : -1;

		*p = end;
		p += (quote && end == '"') ? 1 : 0;

		if (fd < 0) {
			return -1;
		}

		int8_t *stream = input ? &s->in : &s->out;

		/* `> a > b`: a is done with */
		if (*stream != (input ? saved->in : saved->out)) {
			close(*stream);
		}

		*stream = (int8_t)fd;

		if (!input) {
			s->terminal = 0;
		}

		memset(start, ' ', (size_t)(p - start));
		p--;
	}

	return 0;
}
#endif

/* a built-in command, NULL if there is none */
static const vfs_command_t *posix_builtin(const char *name)
{
	for (unsigned int i = 0; i < vfs_commands_count; i++) {
		if (strcmp(name, vfs_commands[i].name) == 0) {
			return &vfs_commands[i];
		}
	}

	return NULL;
}

/* the command node named @name: a path, else any node of that name with an execute handler */
static vfs_node_t *posix_command(const char *name, bool *found, const vfs_command_t **builtin)
{
	vfs_node_t *t = vfs_nodes_get();
	vfs_path_t p;

	*found = false;
	*builtin = NULL;

	if (strchr(name, '/') != NULL) {
		if (vfs_resolve(name, &p) < 0) {
			return NULL;
		}

		if (p.kind == VFS_NONE && p.dir == VFS_BIN) {
			*builtin = posix_builtin(p.name);
		}

		if (p.kind != VFS_NODE) {
			return NULL;
		}

		*found = true;

		return t[p.node].handlers->execute_handler != NULL ? t + p.node : NULL;
	}

	/* plain nodes first, then /opt and the other directories */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < VFS_NODES_MAX; i++) {
			vfs_node_t *n = t + i;

			if (n->handlers == NULL || (((uint8_t)n->name[0] & 0x80u) != 0) != (pass == 1)) {
				continue;
			}

			if (strcmp(name, n->name + pass) != 0) {
				continue;
			}

			*found = true;

			if (n->handlers->execute_handler != NULL) {
				return n;
			}
		}
	}

	*builtin = posix_builtin(name);

	return NULL;
}

static int (*posix_interpreter)(void *args, int argc, char *argv[]);

void vfs_interpreter_set(int (*run)(void *args, int argc, char *argv[]))
{
	posix_interpreter = run;
}

/* the script @name runs: a file or RAM file at a path, or a file in VFS_HOME */
static bool posix_script(const char *name, char *path)
{
	vfs_path_t p;

	if (strchr(name, '/') == NULL) {
		if (strlen(VFS_HOME) + strlen(name) + 2u > VFS_PATH_MAX) {
			return false;
		}

		strcpy(path, VFS_HOME "/");
		strcat(path, name);
	} else {
		strncpy(path, name, VFS_PATH_MAX - 1);
		path[VFS_PATH_MAX - 1] = '\0';
	}

	if (vfs_resolve(path, &p) < 0) {
		return false;
	}

	return p.kind == VFS_FILE ||
	       (p.kind == VFS_NODE && tmp_file_size(vfs_nodes_get() + p.node) >= 0);
}

/* @name is no command: a script, or an error. Apart, to keep its path off the stack of a command */
static int posix_run_script(const char *name, bool found)
{
	char path[VFS_PATH_MAX];

	if (posix_interpreter != NULL && posix_script(name, path)) {
		char *run[3] = {"exec", path, strrchr(path, '/') + 1};

		return posix_interpreter(NULL, 3, run);
	}

	printf(found ? "not executive\n" : "not found\n");

	return -1;
}

/* runs the command in posix_system_string */
static int posix_run(void)
{
	char *argv[POSIX_ARGS_MAX];

	int argc = args_parse(posix_system_string, argv, POSIX_ARGS_MAX);

	if (argc < 1) {
		return -1;
	}

	if (strcmp(argv[0], "reboot") == 0 && argc == 1) {
#if defined(__XC16__)
		__asm__ volatile("reset");
#else
		exit(0); /* host build: the process ends */
#endif
		return 1;
	}

	bool found;
	const vfs_command_t *builtin;
	vfs_node_t *e = posix_command(argv[0], &found, &builtin);

	if (e != NULL) {
		return e->handlers->execute_handler(e->args, argc, argv);
	}

	if (builtin != NULL) {
		return builtin->run(NULL, argc, argv);
	}

	return posix_run_script(argv[0], found);
}

/*
 * Runs a command line: the first word names a command (`ls`, `/opt/trgmon`),
 * whose handler gets the words as argc / argv. A script (a file at a path,
 * or by its name in VFS_HOME) goes to the interpreter set with
 * vfs_interpreter_set(), as a task of its name. `reboot` is built in. With
 * `<`, `>` or `>>` the command, and the tasks it starts, run on those streams
 * (see posix_redirect()). Returns the command's result, or -1 when there is no
 * such command.
 */
int __attribute__((__section__(".libc.system"))) system(const char *input)
{
	if (input == NULL) {
		return -1;
	}

	/* a copy: args_parse() splits the line in place */
	strncpy(posix_system_string, input, POSIX_SYSTEM_SIZE - 1);
	posix_system_string[POSIX_SYSTEM_SIZE - 1] = '\0';

#ifdef VFS_NO_REDIRECT
	return posix_run();
#else
	task_streams_t *s = task_streams();
	task_streams_t saved = *s;

	int r = posix_redirect(posix_system_string, s, &saved);

	if (r == 0) {
		r = posix_run();
	}

	int in = (s->in != saved.in) ? s->in : -1;
	int out = (s->out != saved.out) ? s->out : -1;

	/* not cwd: `cd` is what a command is for */
	s->in = saved.in;
	s->out = saved.out;
	s->terminal = saved.terminal;

	vfs_streams_close(in, out);

	return r;
#endif
}

#if !defined(__XC16__)
/* a PC's printf is not ours: send it where the kernel's would go, when that is not the console */
int k_printf(const char *format, ...)
{
	char text[256];
	va_list list;

	va_start(list, format);

	int n = vsnprintf(text, sizeof(text), format, list);

	va_end(list);

	if (task_streams()->out >= 0 && n > 0) {
		return write(1, text, (unsigned int)((n < (int)sizeof(text)) ? n : (int)sizeof(text) - 1));
	}

	return fputs(text, stdout) < 0 ? -1 : n;
}
#endif
