/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The C library's file and process calls, implemented on the node table:
 * open() finds a node by name, read() / write() / lseek() / close() call the
 * node's handlers, and system() runs a command line. A file handle is simply
 * the node's index in the table; handles 0, 1 and 2 are stdin, stdout and
 * stderr (see fsi_node_standard()).
 *
 * XC16 links these functions instead of its own from the sections named in
 * their attributes.
 */

#include "../fsi/fsi.h"
#include "../lib/args.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */

#define POSIX_STDOUT_SIZE 128u /* stdout line buffer */

static char posix_stdout_buf[POSIX_STDOUT_SIZE];
static unsigned int posix_stdout_pos = 0;

/* the node behind a handle, NULL when the handle is not valid */
static fsi_node_t *posix_node(int handle)
{
	if (handle < 0 || handle >= FSI_MAX_NODES_COUNT) {
		return NULL;
	}

	fsi_node_t *n = fsi_nodes_get() + handle;

	if (n->used != FSI_NODE_USED || n->handlers == NULL) {
		return NULL;
	}

	return n;
}

/*
 * stdout is line buffered: a line is passed on when '\n' arrives or the
 * buffer is full, so a serial port gets whole lines instead of single bytes.
 */
static int posix_stdout_write(fsi_node_t *n, const char *buffer, unsigned int len)
{
	bool crlf = fsi_terminal(); /* a terminal needs "\r\n" to start a line */

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

		if (n->handlers->write_handler(n->args, posix_stdout_buf, size) < 0) {
			return -1;
		}
	}

	return len;
}

/* -------------------------------------------------------------------------- */

/* returns the handle of the node named `pathname`, -1 if there is none */
int __attribute__((__section__(".libc.open"))) open(const char *pathname, int flags, int mode)
{
	if (pathname == NULL) {
		return -1;
	}

	for (int i = 1; i < FSI_MAX_NODES_COUNT; i++) {
		fsi_node_t *n = posix_node(i);

		if (n == NULL || strcmp(n->name, pathname) != 0) {
			continue;
		}

		if (n->handlers->open_handler != NULL) {
			n->handlers->open_handler(pathname, flags, mode);
		}

		return i;
	}

	return -1;
}

int __attribute__((__section__(".libc.close"))) close(int handle)
{
	fsi_node_t *n = posix_node(handle);

	if (n == NULL) {
		return -1;
	}

	if (n->handlers->close_handler == NULL) {
		return 0;
	}

	return n->handlers->close_handler(handle);
}

int __attribute__((__section__(".libc.write"))) write(int handle, void *buffer, unsigned int len)
{
	fsi_node_t *n = posix_node(handle);

	if (n == NULL || buffer == NULL || n->handlers->write_handler == NULL) {
		return -1;
	}

	if (handle == 1) {
		return posix_stdout_write(n, buffer, len);
	}

	return n->handlers->write_handler(n->args, buffer, len);
}

int __attribute__((__section__(".libc.read"))) read(int handle, void *buffer, unsigned int len)
{
	fsi_node_t *n = posix_node(handle);

	if (n == NULL || buffer == NULL || n->handlers->read_handler == NULL) {
		return -1;
	}

	return n->handlers->read_handler(n->args, buffer, len);
}

off_t __attribute__((__section__(".libc.lseek"))) lseek(int handle, off_t offset, int whence)
{
	fsi_node_t *n = posix_node(handle);

	if (n == NULL || n->handlers->lseek_handler == NULL) {
		return -1;
	}

	return n->handlers->lseek_handler(handle, offset, whence);
}

/* -------------------------------------------------------------------------- */

#define POSIX_SYSTEM_SIZE 128u /* longest command line */
#define POSIX_ARGS_MAX	  16   /* most words in a command line */

static char posix_system_string[POSIX_SYSTEM_SIZE];

/*
 * Runs a command line: the first word names a command node (see
 * fsi_register_app()), whose handler gets the words as argc / argv.
 * `reboot` is built in. Returns the command's result, or -1 when there is no
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

	fsi_node_t *e = NULL;

	for (int i = 2; i < FSI_MAX_NODES_COUNT; i++) {
		fsi_node_t *n = posix_node(i);

		if (n != NULL && strcmp(argv[0], n->name) == 0) {
			e = n;
			break;
		}
	}

	if (e == NULL) {
		printf("not found\n");
		return -1;
	}

	if (e->handlers->execute_handler == NULL) {
		printf("not executive\n");
		return -1;
	}

	return e->handlers->execute_handler(e->args, argc, argv);
}
