/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file lets/node.h
 * @brief A program that offers a node (a device, as `date` does) or a command: the part of
 * vfs/vfs.h it needs, with standard headers only. Drivers and board code use vfs/vfs.h.
 */
#ifndef LETS_INCLUDE_LETS_NODE_H
#define LETS_INCLUDE_LETS_NODE_H

#include <stdbool.h>
#include <unistd.h> /* off_t */

/** What a node can do. Any handler may be NULL. Each gets the node's args. */
typedef struct {
	/** Called by open(); the result is ignored. */
	int (*open_handler)(void *args, int flags);
	/** Writes @p length bytes; returns the bytes written or -1. */
	int (*write_handler)(void *args, void *buffer, unsigned int length);
	/** Reads up to @p length bytes; returns the bytes read or -1. */
	int (*read_handler)(void *args, void *buffer, unsigned int length);
	/** Moves the read position, as lseek(). */
	int (*lseek_handler)(void *args, off_t offset, int whence);
	/** Called by close() of the last handle on the node. */
	int (*close_handler)(void *args);
	/** Runs the node as a command (see system()). */
	int (*execute_handler)(void *args, int argc, char *argv[]);
	/**
	 * Optional: which of @p events (POLLIN, POLLOUT) the node is ready for now, for poll(); a
	 * node without one is always ready. Called often while a task waits: keep it short.
	 */
	int (*poll_handler)(void *args, int events);
} vfs_handler_t;

/**
 * @brief Creates node @p path ("/dev/x" or a plain name) with @p handlers, given @p args.
 * @return its index, or -1 (no free entry, a bad name).
 */
int vfs_node_create(const char *path, bool hidden, void *args, const vfs_handler_t *handlers);

/** @brief Registers a command (`/opt/NAME`, or a plain name in /bin). @return 0 or -1. */
int vfs_register_app(const char *path, int (*execute_handler)(void *args, int argc, char *argv[]));

#endif
