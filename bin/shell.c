/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The shell: a task that reads command lines from stdin and runs them.
 *
 * Two modes, chosen by `std`:
 *
 * - std <node>: a link to a program. Whatever one read() returns is one
 *   command line, run as it is, with no echo and no prompt, so a program
 *   reads only the replies.
 * - std -t <node>: a terminal with a person typing. Keys are collected into
 *   a line and echoed; Enter runs it. Backspace, Ctrl-U (clear the line),
 *   Ctrl-C (drop it), Up / Down (earlier lines) and Tab (complete a name:
 *   commands, devices, files in the node table, or the last part of a path)
 *   work as in a PC shell.
 */

#include "../fsi/fsi.h"

#include "../kernel/task.h"

#include "kill.h"
#include "shell.h"
#include "sscript.h"
#include "tmp.h"
#include "top.h"

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

/* keys */
#define KEY_CTRL_C    0x03
#define KEY_BACKSPACE 0x08
#define KEY_TAB	      0x09
#define KEY_CTRL_U    0x15
#define KEY_ESC	      0x1B
#define KEY_DELETE    0x7F

static char shell_buffer[SHELL_BUFFER_SIZE];

/* the line being typed in terminal mode */
static struct {
	unsigned int length; /* characters in shell_buffer */
	bool prompted;	     /* the prompt for this line is shown */
	bool last_cr;	     /* the previous key was '\r' (a "\r\n" Enter) */
	uint8_t escape;	     /* 0, or 1 after ESC, 2 after ESC [ */
#if SHELL_HISTORY > 0
	char (*history)[SHELL_BUFFER_SIZE]; /* [0] is the newest; from the heap, on first use */
	int recalled;			    /* 0: the line is new, k: history[k - 1] is shown */
#endif
} shell;

/* the echo goes to stderr: the same terminal as stdout, but not line buffered */
static void shell_print(const char *text)
{
	write(2, (void *)text, strlen(text));
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

static void shell_prompt(void)
{
	shell_prompt_print();
	shell.prompted = true;
}

/* replaces what is shown after the prompt with the line in shell_buffer */
static void shell_redraw(unsigned int shown)
{
	shell_print("\r");
	shell_prompt_print();
	write(2, shell_buffer, shell.length);

	/* blank out what is left of a longer line, then step back */
	for (unsigned int i = shell.length; i < shown; i++) {
		shell_print(" ");
	}
	for (unsigned int i = shell.length; i < shown; i++) {
		shell_print("\b");
	}
}

/* -------------------------------------------------------------------------- */
/* Tab: complete the word under the cursor with the names in the node table */

/* the name of node @p i as typed, NULL for an unused node */
static const char *shell_node_name(int i)
{
	fsi_node_t *n = fsi_nodes_get() + i;

	if (n->used != FSI_NODE_USED || n->handlers == NULL || n->name[0] == '\0') {
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
	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		const char *name = shell_node_name(i);

		if (name == NULL) {
			continue;
		}

		if (command && fsi_nodes_get()[i].handlers->execute_handler == NULL) {
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
	char first[FSI_NODE_NAME_MAX_LENGTH]; /* the first match */
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
	unsigned int word = shell.length;

	while (word > 0 && shell_buffer[word - 1] != ' ') {
		word--;
	}

	/* the part to complete: after the last '/' of the word */
	unsigned int part = shell.length;

	while (part > word && shell_buffer[part - 1] != '/') {
		part--;
	}

	shell_buffer[shell.length] = '\0';

	memset(&shell_tab, 0, sizeof(shell_tab));
	shell_tab.typed = shell_buffer + part;
	shell_tab.length = shell.length - part;

	/* the first word, without a path, is a command */
	bool command = (word == 0 && part == word);

	shell_names(shell_buffer + word, command, shell_candidate);

	if (shell_tab.matches == 0) {
		return;
	}

	if (shell_tab.matches == 1 || shell_tab.common > shell_tab.length) {
		/* add the shared part; after a whole name a space, or '/' for a directory */
		unsigned int add = shell_tab.common - shell_tab.length;
		const char *rest = shell_tab.first + shell_tab.length;

		if (shell.length + add + 1 >= SHELL_BUFFER_SIZE) {
			return;
		}

		memcpy(shell_buffer + shell.length, rest, add);
		shell.length += add;
		write(2, (void *)rest, add);

		if (shell_tab.matches == 1) {
			char end = shell_tab.dir ? '/' : ' ';

			shell_buffer[shell.length++] = end;
			write(2, &end, 1);
		}
		return;
	}

	/* nothing more in common: list the choices, then the line again */
	shell_print("\r\n");
	shell_tab.listing = true;
	shell_names(shell_buffer + word, command, shell_candidate);
	shell_print("\r\n");
	shell_redraw(0);
}

/* -------------------------------------------------------------------------- */
/* Up / Down */

#if SHELL_HISTORY > 0
static void shell_history_add(void)
{
	if (shell.history == NULL || shell.length == 0 ||
	    strcmp(shell.history[0], shell_buffer) == 0) {
		return;
	}

	memmove(shell.history[1], shell.history[0], (SHELL_HISTORY - 1) * SHELL_BUFFER_SIZE);
	strcpy(shell.history[0], shell_buffer);
}

static void shell_history_show(int step)
{
	int entry = shell.recalled + step;

	if (shell.history == NULL || entry < 0 || entry > SHELL_HISTORY) {
		return;
	}

	if (entry > 0 && shell.history[entry - 1][0] == '\0') {
		return;
	}

	unsigned int shown = shell.length;

	shell.recalled = entry;

	if (entry == 0) {
		shell.length = 0; /* below the newest entry: an empty line */
	} else {
		strcpy(shell_buffer, shell.history[entry - 1]);
		shell.length = strlen(shell_buffer);
	}

	shell_redraw(shown);
}
#endif

/* -------------------------------------------------------------------------- */

static void shell_enter(void)
{
	shell_print("\r\n");
	shell_buffer[shell.length] = '\0';

#if SHELL_HISTORY > 0
	shell_history_add();
	shell.recalled = 0;
#endif

	unsigned int length = shell.length;

	shell.length = 0;
	shell.prompted = false;

	if (length > 0) {
		system(shell_buffer);
	}
}

/* one key typed on the terminal */
static void shell_key(char c)
{
	if (shell.escape == 1) {
		shell.escape = (c == '[') ? 2 : 0;
		return;
	}

	if (shell.escape == 2) {
		shell.escape = 0;
#if SHELL_HISTORY > 0
		if (c == 'A') {
			shell_history_show(1);
		} else if (c == 'B') {
			shell_history_show(-1);
		}
#endif
		return; /* other keys (Left, Right, ...) are ignored */
	}

	bool after_cr = shell.last_cr;

	shell.last_cr = (c == '\r');

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
		if (shell.length > 0) {
			shell.length--;
			shell_print("\b \b");
		}
		break;
	case KEY_CTRL_U: {
		unsigned int shown = shell.length;

		shell.length = 0;
		shell_redraw(shown);
		break;
	}
	case KEY_CTRL_C:
		shell_print("^C\r\n");
		shell.length = 0;
		shell.prompted = false;
		break;
	case KEY_TAB:
		shell_complete();
		break;
	case KEY_ESC:
		shell.escape = 1;
		break;
	default:
		if ((unsigned char)c >= ' ' && shell.length < SHELL_BUFFER_SIZE - 1) {
			shell_buffer[shell.length++] = c;
			write(2, &c, 1);
		}
		break;
	}
}

/* the shell task: polls stdin every 10 ms */
int shell_loop(void *args, uint64_t system_time)
{
	char *buf = args;

	if (fsi_terminal()) {
		char keys[16];

		if (!shell.prompted) {
#if SHELL_HISTORY > 0
			if (shell.history == NULL) {
				/* only a terminal needs it; owned by the shell task, kept */
				shell.history = malloc(SHELL_HISTORY * SHELL_BUFFER_SIZE);

				if (shell.history != NULL) {
					memset(shell.history, 0, SHELL_HISTORY * SHELL_BUFFER_SIZE);
				}
			}
#endif
			shell_prompt();
		}

		int size = read(0, keys, sizeof(keys));

		for (int i = 0; i < size; i++) {
			shell_key(keys[i]);

			if (!shell.prompted && fsi_terminal()) {
				shell_prompt(); /* the command is done: the next line */
			}
		}

		return 10;
	}

	/* a link to a program: one read is one command line */
	shell.prompted = false;

	int size = read(0, buf, SHELL_BUFFER_SIZE - 1);

	if (size < 1) {
		return 10;
	}

	/* drop the line ending: terminals send \r, \n or both */
	while (size > 0 && (buf[size - 1] == '\n' || buf[size - 1] == '\r')) {
		size--;
	}

	buf[size] = '\0';

	if (size > 0) {
		system(buf);
	}

	return 10;
}

/*
 * ls, cat, echo and mv are weak: a project that replaces them (vfs) can leave
 * their files out of the build, and they are then not registered here.
 */
extern int cat(void *args, int argc, char *argv[]) __attribute__((weak));
extern int echo(void *args, int argc, char *argv[]) __attribute__((weak));
extern int ls(void *args, int argc, char *argv[]) __attribute__((weak));
extern int mv(void *args, int argc, char *argv[]) __attribute__((weak));

int shell_initialize(void *args)
{
	if (cat != NULL) {
		fsi_register_app("cat", cat);
	}

	if (echo != NULL) {
		fsi_register_app("echo", echo);
	}

	if (ls != NULL) {
		fsi_register_app("ls", ls);
	}

	if (mv != NULL) {
		fsi_register_app("mv", mv);
	}

	fsi_register_app("kill", kill);
	fsi_register_app("top", top);
	fsi_register_app("touch", tmp_file_create);
	fsi_register_app("rm", tmp_file_delete);

	task_create("shell", shell_loop, (void *)shell_buffer);

	if (args == NULL) {
		return 0;
	}

	task_create("startup", sscript, args);

	return 0;
}
