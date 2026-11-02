/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The shell: a task that reads command lines from stdin and runs them.
 *
 * shell_initialize() starts one on the console, and it goes where `std` points.
 * The command `shell` starts one on streams of its own, as many as memory allows:
 *
 *   shell -t < ttyS1 > ttyS1
 *
 * Two modes:
 *
 * - shell, std <node>: a link to a program. Lines end with '\n' or '\r', so a
 *   file can be read and a line can come in pieces. One with no end runs when
 *   the input stops for a moment, as programs that send a line per packet do.
 *   No echo and no prompt, so a program reads only the replies.
 * - shell -t, std -t <node>: a terminal with a person typing. Keys are collected
 *   into a line and echoed; Enter runs it. Backspace, Ctrl-U (clear the line),
 *   Ctrl-C (drop it), Up / Down (earlier lines) and Tab (complete a name:
 *   commands, devices, files in the node table, or the last part of a path)
 *   work as in a PC shell.
 */

#include "../vfs/vfs.h"

#include "../kernel/task.h"

#include "shell.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define SHELL_BUFFER_SIZE 128 /* longest command line */

#ifndef SHELL_HISTORY
#define SHELL_HISTORY 2 /* earlier lines kept for Up / Down (0 = none), 128 B each */
#endif

#define SHELL_PROMPT "> "
#define SHELL_LINE_WAIT 5 /* polls with no input before a line with no end runs */

/* keys */
#define KEY_CTRL_C    0x03
#define KEY_BACKSPACE 0x08
#define KEY_TAB	      0x09
#define KEY_CTRL_U    0x15
#define KEY_ESC	      0x1B
#define KEY_DELETE    0x7F

/*
 * The echo goes to stderr: the same terminal as stdout, but not line buffered.
 * It is collected and written once per batch of keys: on a DRN port each write
 * is a packet, and a burst of one-byte packets overflows the port's queue.
 */
#ifndef SHELL_OUT_SIZE
#define SHELL_OUT_SIZE 64
#endif

typedef struct {
	char buffer[SHELL_BUFFER_SIZE]; /* the line */
	unsigned int length; /* characters in buffer */
	bool started;	     /* it has run */
	bool prompted;	     /* the prompt for this line is shown */
	bool last_cr;	     /* the previous key was '\r' (a "\r\n" Enter) */
	uint8_t escape;	     /* 0, or 1 after ESC, 2 after ESC [ */
	uint8_t idle;	     /* polls with no input, on a link */
	bool in_command;     /* a command typed here runs now: shell_wait() is for this shell */
	task_t *wait;	     /* a task a command left running: no prompt, no input until it ends */
	int (*wait_entry)(void *arg, uint64_t system_time); /* its function: the slot may be reused */
#if SHELL_HISTORY > 0
	char (*history)[SHELL_BUFFER_SIZE]; /* [0] is the newest; from the heap, on first use */
	int recalled;			    /* 0: the line is new, k: history[k - 1] is shown */
#endif
	char out[SHELL_OUT_SIZE];
	unsigned int out_length;
} shell_t;

static shell_t shell_console;		   /* the one shell_initialize() starts */
static shell_t *shell = &shell_console;	   /* the one running */
static bool shell_console_idle;		   /* it is there, and no console yet */

static void shell_flush(void)
{
	if (shell->out_length > 0) {
		write(2, shell->out, shell->out_length);
		shell->out_length = 0;
	}
}

void shell_write(const void *data, unsigned int length)
{
	const char *p = data;

	while (length > 0) {
		unsigned int n = SHELL_OUT_SIZE - shell->out_length;

		if (n > length) {
			n = length;
		}

		memcpy(shell->out + shell->out_length, p, n);
		shell->out_length += n;
		p += n;
		length -= n;

		if (shell->out_length == SHELL_OUT_SIZE) {
			shell_flush();
		}
	}
}

void shell_wait(task_t *t)
{
	if (t != NULL && shell->in_command) {
		shell->wait = t;
		shell->wait_entry = t->entry;
	}
}

/* runs a command typed here */
static void shell_run(char *line)
{
	shell->in_command = true;
	system(line);
	shell->in_command = false;
}

/* a command's task still runs: true while the shell waits for it */
static bool shell_waiting(void)
{
	task_t *t = shell->wait;

	if (t == NULL) {
		return false;
	}

	if (t->state == TASK_UNUSED || t->entry != shell->wait_entry) {
		shell->wait = NULL;
		return false;
	}

	return true;
}

static void shell_print(const char *text)
{
	shell_write(text, strlen(text));
}

static void shell_prompt_default(void)
{
	shell_print(SHELL_PROMPT);
}

static void (*shell_prompt_print)(void) = shell_prompt_default;

void shell_prompt_set(void (*prompt)(void))
{
	shell_prompt_print = (prompt != NULL) ? prompt : shell_prompt_default;
}

#ifndef SHELL_NO_TERMINAL

static void shell_prompt(void)
{
	shell_prompt_print();
	shell->prompted = true;
}

/* replaces what is shown after the prompt with the line in shell->buffer */
static void shell_redraw(unsigned int shown)
{
	shell_print("\r");
	shell_prompt_print();
	shell_write(shell->buffer, shell->length);

	/* blank out what is left of a longer line, then step back */
	for (unsigned int i = shell->length; i < shown; i++) {
		shell_print(" ");
	}
	for (unsigned int i = shell->length; i < shown; i++) {
		shell_print("\b");
	}
}

/* -------------------------------------------------------------------------- */
/* Tab: complete the word under the cursor with the names in the node table */

/* the name of node @p i as typed, NULL for an unused node */
static const char *shell_node_name(int i)
{
	vfs_node_t *n = vfs_nodes_get() + i;

	if (n->handlers == NULL || n->name[0] == '\0') {
		return NULL;
	}

	const char *name = n->name;

	if ((uint8_t)name[0] & 0x80) {
		name++; /* in a vfs directory: the first byte is the directory */
	}

	return name;
}

void shell_node_names(const char *word, bool command, bool (*candidate)(const char *, bool))
{
	for (unsigned int i = 0; command && i < vfs_commands_count; i++) {
		candidate(vfs_commands[i].name, false);
	}

	for (int i = 0; i < VFS_NODES_MAX; i++) {
		const char *name = shell_node_name(i);

		if (name == NULL) {
			continue;
		}

		if (command && vfs_nodes_get()[i].handlers->execute_handler == NULL) {
			continue; /* the first word is run: only what executes */
		}

		candidate(name, false);
	}
}

static shell_names_t shell_names = shell_node_names;

void shell_names_set(shell_names_t names)
{
	shell_names = (names != NULL) ? names : shell_node_names;
}

/* what Tab found so far: the names that start with what is typed */
static struct {
	const char *typed;   /* the part being completed (after the last ' ' or '/') */
	unsigned int length; /* its characters */
	bool listing;	     /* second pass: print the names */
	int matches;	     /* names that match */
	unsigned int common; /* characters all of them share */
	bool dir;	     /* the one match is a directory */
	char first[VFS_NODE_NAME_MAX]; /* the first match */
} shell_tab;

static bool shell_candidate(const char *name, bool dir)
{
	if (strncmp(name, shell_tab.typed, shell_tab.length) != 0) {
		return false;
	}

	if (shell_tab.listing) {
		shell_print(name);
		shell_print(dir ? "/  " : "  ");
		return true;
	}

	if (shell_tab.matches++ == 0) {
		strncpy(shell_tab.first, name, sizeof(shell_tab.first) - 1);
		shell_tab.first[sizeof(shell_tab.first) - 1] = '\0';
		shell_tab.common = strlen(shell_tab.first);
		shell_tab.dir = dir;
	} else {
		unsigned int k = shell_tab.length;

		while (k < shell_tab.common && name[k] == shell_tab.first[k]) {
			k++;
		}
		shell_tab.common = k;
	}

	return true;
}

static void shell_complete(void)
{
	/* the word: from the last space to the end of the line */
	unsigned int word = shell->length;

	while (word > 0 && shell->buffer[word - 1] != ' ') {
		word--;
	}

	/* the part to complete: after the last '/' of the word */
	unsigned int part = shell->length;

	while (part > word && shell->buffer[part - 1] != '/') {
		part--;
	}

	shell->buffer[shell->length] = '\0';

	memset(&shell_tab, 0, sizeof(shell_tab));
	shell_tab.typed = shell->buffer + part;
	shell_tab.length = shell->length - part;

	/* the first word, without a path, is a command */
	bool command = (word == 0 && part == word);

	shell_names(shell->buffer + word, command, shell_candidate);

	if (shell_tab.matches == 0) {
		return;
	}

	if (shell_tab.matches == 1 || shell_tab.common > shell_tab.length) {
		/* add the shared part; after a whole name a space, or '/' for a directory */
		unsigned int add = shell_tab.common - shell_tab.length;
		const char *rest = shell_tab.first + shell_tab.length;

		if (shell->length + add + 1 >= SHELL_BUFFER_SIZE) {
			return;
		}

		memcpy(shell->buffer + shell->length, rest, add);
		shell->length += add;
		shell_write(rest, add);

		if (shell_tab.matches == 1) {
			char end = shell_tab.dir ? '/' : ' ';

			shell->buffer[shell->length++] = end;
			shell_write(&end, 1);
		}
		return;
	}

	/* nothing more in common: list the choices, then the line again */
	shell_print("\r\n");
	shell_tab.listing = true;
	shell_names(shell->buffer + word, command, shell_candidate);
	shell_print("\r\n");
	shell_redraw(0);
}

/* -------------------------------------------------------------------------- */
/* Up / Down */

#if SHELL_HISTORY > 0
static void shell_history_add(void)
{
	if (shell->history == NULL || shell->length == 0 ||
	    strcmp(shell->history[0], shell->buffer) == 0) {
		return;
	}

	memmove(shell->history[1], shell->history[0], (SHELL_HISTORY - 1) * SHELL_BUFFER_SIZE);
	strcpy(shell->history[0], shell->buffer);
}

static void shell_history_show(int step)
{
	int entry = shell->recalled + step;

	if (shell->history == NULL || entry < 0 || entry > SHELL_HISTORY) {
		return;
	}

	if (entry > 0 && shell->history[entry - 1][0] == '\0') {
		return;
	}

	unsigned int shown = shell->length;

	shell->recalled = entry;

	if (entry == 0) {
		shell->length = 0; /* below the newest entry: an empty line */
	} else {
		strcpy(shell->buffer, shell->history[entry - 1]);
		shell->length = strlen(shell->buffer);
	}

	shell_redraw(shown);
}
#endif

/* -------------------------------------------------------------------------- */

static void shell_enter(void)
{
	shell_print("\r\n");
	shell->buffer[shell->length] = '\0';

#if SHELL_HISTORY > 0
	shell_history_add();
	shell->recalled = 0;
#endif

	unsigned int length = shell->length;

	shell->length = 0;
	shell->prompted = false;

	/* an empty line: the new line and the prompt go out together */
	if (length > 0) {
		shell_flush(); /* the line ends before the command's output */
		shell_run(shell->buffer);
	}
}

/* one key typed on the terminal */
static void shell_key(char c)
{
	if (shell->escape == 1) {
		shell->escape = (c == '[') ? 2 : 0;
		return;
	}

	if (shell->escape == 2) {
		shell->escape = 0;
#if SHELL_HISTORY > 0
		if (c == 'A') {
			shell_history_show(1);
		} else if (c == 'B') {
			shell_history_show(-1);
		}
#endif
		return; /* other keys (Left, Right, ...) are ignored */
	}

	bool after_cr = shell->last_cr;

	shell->last_cr = (c == '\r');

	switch (c) {
	case '\r':
		shell_enter();
		break;
	case '\n':
		if (!after_cr) {
			shell_enter(); /* a terminal that sends only '\n' */
		}
		break;
	case KEY_BACKSPACE:
	case KEY_DELETE:
		if (shell->length > 0) {
			shell->length--;
			shell_print("\b \b");
		}
		break;
	case KEY_CTRL_U: {
		unsigned int shown = shell->length;

		shell->length = 0;
		shell_redraw(shown);
		break;
	}
	case KEY_CTRL_C:
		shell_print("^C\r\n");
		shell->length = 0;
		shell->prompted = false;
		break;
	case KEY_TAB:
		shell_complete();
		break;
	case KEY_ESC:
		shell->escape = 1;
		break;
	default:
		if ((unsigned char)c >= ' ' && shell->length < SHELL_BUFFER_SIZE - 1) {
			shell->buffer[shell->length++] = c;
			shell_write(&c, 1);
		}
		break;
	}
}

#else

void shell_node_names(const char *word, bool command, bool (*candidate)(const char *, bool))
{
}

void shell_names_set(shell_names_t names)
{
}

#endif /* SHELL_NO_TERMINAL */

/* runs the lines in the buffer; @p all: also what has no line end */
static void shell_lines(bool all)
{
	char *buf = shell->buffer;

	while (shell->length > 0 && !task_stopping()) {
		unsigned int n = 0;

		while (n < shell->length && buf[n] != '\n' && buf[n] != '\r') {
			n++;
		}

		/* a full buffer has to go too: nothing can come after it */
		if (n == shell->length && !all && shell->length < SHELL_BUFFER_SIZE - 1) {
			return;
		}

		buf[n] = '\0';

		if (n > 0) {
			shell_run(buf);
		}

		n += (n < shell->length) ? 1u : 0u;
		shell->length -= n;
		memmove(buf, buf + n, shell->length);

		/* the command left a task running: the next lines wait for it */
		if (shell_waiting()) {
			return;
		}
	}
}

/* the shell task: polls stdin every 10 ms */
int shell_loop(void *args, uint64_t system_time)
{
	if (args != NULL) {
		shell = args;
	}

	/* as a login: rc's `cd` is rc's own */
	if (!shell->started) {
		shell->started = true;
		vfs_chdir(VFS_HOME);
	}

	/*
	 * A command's task still writes: no prompt and no input until it ends,
	 * so its output comes before the prompt. Ctrl-C on a terminal stops it.
	 */
	if (shell_waiting()) {
#ifndef SHELL_NO_TERMINAL
		if (vfs_terminal()) {
			char keys[8];
			int size = read(0, keys, sizeof(keys));

			if (size > 0 && memchr(keys, 0x03, (size_t)size) != NULL) {
				task_stop_byname(shell->wait->info.name);
				shell->wait = NULL;
			}
		}
#endif
		return 10;
	}

	/* lines left behind by a wait */
	if (!vfs_terminal() && shell->length > 0) {
		shell_lines(false);

		if (shell_waiting()) {
			return 10;
		}
	}

#ifndef SHELL_NO_TERMINAL
	if (vfs_terminal()) {
		char keys[16];

		if (!shell->prompted) {
#if SHELL_HISTORY > 0
			if (shell->history == NULL) {
				/* only a terminal needs it; freed with the shell */
				shell->history = task_alloc(SHELL_HISTORY * SHELL_BUFFER_SIZE);

				if (shell->history != NULL) {
					memset(shell->history, 0, SHELL_HISTORY * SHELL_BUFFER_SIZE);
				}
			}
#endif
			shell_prompt();
		}

		int size = read(0, keys, sizeof(keys));

		for (int i = 0; i < size && !task_stopping(); i++) {
			shell_key(keys[i]);

			/* a command left a task running: the prompt comes when it ends */
			if (shell_waiting()) {
				break;
			}

			if (!shell->prompted && vfs_terminal() && !task_stopping()) {
				shell_prompt(); /* the command is done: the next line */
			}
		}

		shell_flush();

		return 10;
	}

#endif

	shell->prompted = false;

	int size = read(0, shell->buffer + shell->length, SHELL_BUFFER_SIZE - 1 - shell->length);

	if (size > 0) {
		shell->length += (unsigned int)size;
		shell->idle = 0;
		shell_lines(false);
	} else if (shell->length > 0 && ++shell->idle >= SHELL_LINE_WAIT) {
		shell_lines(true);
	}

	return 10;
}

#ifndef VFS_NO_REDIRECT
/* shell [-t] < in > out: a shell on these streams, until `kill shell` */
int shell_start(void *args, int argc, char *argv[])
{
	bool terminal = (argc == 2 && strcmp(argv[1], "-t") == 0);

	if (argc > 2 || (argc == 2 && !terminal) || task_streams()->in < 0) {
		printf("usage: shell [-t] < in > out\n");
		return -1;
	}

	shell_t *s = &shell_console;

	/* nobody is on the console: this shell takes over the memory of that one */
	if (shell_console_idle && vfs_console() < 0) {
		shell_console_idle = false;
		task_stop_byname("shell");
	} else {
		s = malloc(sizeof(shell_t));

		if (s == NULL) {
			printf("out of memory\n");
			return -1;
		}
	}

	memset(s, 0, sizeof(shell_t));

	task_t *t = task_create("shell", shell_loop, s);

	if (t == NULL) {
		if (s != &shell_console) {
			free(s);
		}

		printf("out of memory\n");
		return -1;
	}

	t->streams.terminal = terminal ? 1 : 0;

	return 0;
}
#endif

int shell_initialize(void)
{
#ifndef VFS_NO_REDIRECT
	shell_console_idle = true;
#endif

	task_create("shell", shell_loop, &shell_console);

	return 0;
}
