/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The C library's file and process calls with paths: the vfs version of
 * kernel/posix.c. A project links one of the two. Everything kernel/posix.c
 * does works the same here (bare node names, handles = node indices, line
 * buffered stdout, `reboot`); in addition open() resolves paths and opens
 * files of mounted file systems, and system() runs commands by path.
 */

#include "vfs.h"
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

/* stdout is line buffered, as in kernel/posix.c */
static int posix_stdout_write(fsi_node_t *n, const char *buffer, unsigned int len)
{
	for (unsigned int i = 0; i < len; i++) {
		posix_stdout_buf[posix_stdout_pos++] = buffer[i];

		if (buffer[i] != '\n' && posix_stdout_pos < POSIX_STDOUT_SIZE) {
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

/*
 * Returns the handle of the node at `pathname`. A file of a mounted file
 * system is opened (see vfs_file_open()); O_CREAT in a table directory
 * creates a RAM file, as `touch`.
 */
int __attribute__((__section__(".libc.open"))) open(const char *pathname, int flags, int mode)
{
	vfs_path_t p;

	if (vfs_resolve(pathname, &p) < 0) {
		return -1;
	}

	if (p.kind == VFS_NODE) {
		fsi_node_t *n = posix_node(p.node);

		if (n == NULL) {
			return -1;
		}

		if (n->handlers->open_handler != NULL) {
			n->handlers->open_handler(pathname, flags, mode);
		}

		return p.node;
	}

	if (p.kind == VFS_FILE || (p.kind == VFS_NONE && vfs_mount_get(p.dir) != NULL)) {
		return vfs_file_open(&p, flags);
	}

	if (p.kind == VFS_NONE && (flags & O_CREAT)) {
		return vfs_tmp_create(&p);
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

/* files: the full 32-bit position (the fsi lseek handler returns an int) */
off_t __attribute__((__section__(".libc.lseek"))) lseek(int handle, off_t offset, int whence)
{
	tfs_file_t *f = vfs_file_get(handle);

	if (f != NULL) {
		return (off_t)tfs_seek(f, (int32_t)offset, whence);
	}

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

/* the command named @name: a path, else any node of that name with an execute handler */
static fsi_node_t *posix_command(const char *name, bool *found)
{
	fsi_node_t *t = fsi_nodes_get();
	vfs_path_t p;

	*found = false;

	if (strchr(name, '/') != NULL) {
		if (vfs_resolve(name, &p) < 0 || p.kind != VFS_NODE) {
			return NULL;
		}

		*found = true;

		return t[p.node].handlers->execute_handler != NULL ? t + p.node : NULL;
	}

	/* plain nodes first (fsi), then /app and the other directories */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 2; i < FSI_MAX_NODES_COUNT; i++) {
			fsi_node_t *n = posix_node(i);

			if (n == NULL || (((uint8_t)n->name[0] & 0x80u) != 0) != (pass == 1)) {
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

	return NULL;
}

/*
 * Runs a command line: the first word names a command (`ls`, `/app/trgmon`),
 * whose handler gets the words as argc / argv. `reboot` is built in. Returns
 * the command's result, or -1 when there is no such command.
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

	bool found;
	fsi_node_t *e = posix_command(argv[0], &found);

	if (e == NULL) {
		printf(found ? "not executive\n" : "not found\n");
		return -1;
	}

	return e->handlers->execute_handler(e->args, argc, argv);
}
