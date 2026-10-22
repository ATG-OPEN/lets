/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file fsi.h
 * @brief The node table: everything is a named node.
 *
 * A node is a name plus a set of handlers. A device driver (UART, ADC, ...),
 * a file in RAM and a shell command are all nodes, and all are reached with
 * the usual calls:
 *
 *   open("ttyS1", ...)   finds the node by name and returns its index
 *   read() / write()     call the node's read / write handler
 *   system("ls -a")      calls the execute handler of the node named "ls"
 *
 * The first three entries are stdin, stdout and stderr. They are not separate
 * devices: fsi_node_standard() points them at another node, which is how
 * `std ttyS1` sends all output to a serial port.
 *
 * A handler that is not needed is NULL; the matching call then fails.
 */

#ifndef FSI_H
#define FSI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>

#define FSI_MAX_NODES_COUNT	 40 /**< Size of the node table. */
#define FSI_NODE_NAME_MAX_LENGTH 16 /**< Name length, including the 0. */

/** Whether a table entry holds a node. */
enum FSI_NODE_STATE {
	FSI_NODE_UNUSED = 0,
	FSI_NODE_USED = 1
};

/** What a node can do. Any handler may be NULL. */
typedef struct {
	/** Called by open(); the result is ignored. */
	int (*open_handler)(const char *pathname, int flags, int mode);
	/** Writes @p length bytes; returns the bytes written or -1. */
	int (*write_handler)(void *args, void *buffer, unsigned int length);
	/** Reads up to @p length bytes; returns the bytes read or -1. */
	int (*read_handler)(void *args, void *buffer, unsigned int length);
	/** Moves the read / write position, as lseek(). */
	int (*lseek_handler)(int handle, off_t offset, int whence);
	/** Called by close(). */
	int (*close_handler)(int handle);
	/** Runs the node as a command (see system()). */
	int (*execute_handler)(void *args, int argc, char *argv[]);
} fsi_handler_t;

/** One entry of the node table. */
typedef struct {
	int used;			     /**< FSI_NODE_USED or _UNUSED. */
	bool hidden;			     /**< Not listed by plain `ls`. */
	char name[FSI_NODE_NAME_MAX_LENGTH]; /**< Name used by open(). */
	void *args;			     /**< Passed to the handlers. */
	fsi_handler_t *handlers;	     /**< What the node can do. */
} fsi_node_t;

/** @brief Returns the node table (FSI_MAX_NODES_COUNT entries). */
fsi_node_t *fsi_nodes_get(void);

/**
 * @brief Clears the table and registers the built-in `std` command.
 * @return 0.
 */
int fsi_initialize(void);

/**
 * @brief Registers a command: a hidden node with only an execute handler.
 *
 * @code
 * static int hello(void *args, int argc, char *argv[])
 * {
 *         printf("hello %s\n", argc > 1 ? argv[1] : "world");
 *         return 0;
 * }
 *
 * fsi_register_app("hello", hello);    // the shell now knows `hello`
 * @endcode
 *
 * @return 0, or -1 when the table or the heap is full.
 */
int fsi_register_app(char *name, int (*execute_handler)(void *args, int argc, char *argv[]));

/**
 * @brief The `std <node>` command: redirects stdin / stdout / stderr.
 * @return 0, or -1 when the node does not exist.
 */
int fsi_node_std_execute(void *args, int argc, char *argv[]);

/**
 * @brief Points stdin (0), stdout (1) and stderr (2) at node @p fd.
 *
 * stdin reads with the node's read handler, stdout and stderr write with
 * its write handler.
 *
 * @return 0, or -1 when @p fd is not a node.
 */
int fsi_node_standard(int fd);

/**
 * @brief Adds a node.
 * @param name     Name; longer names are cut to FSI_NODE_NAME_MAX_LENGTH - 1.
 * @param hidden   true: listed only by `ls -a`.
 * @param args     Passed to every handler; stays owned by the caller.
 * @param handlers What the node can do; must stay valid while it exists.
 * @return 0, or -1 when the table is full.
 */
int fsi_node_create(const char *name, bool hidden, void *args, fsi_handler_t *handlers);

/**
 * @brief Removes the node named @p name.
 *
 * The node's args stay owned by the caller: free them afterwards if they
 * were allocated.
 *
 * @return 0, or -1 when there is no such node.
 */
int fsi_node_delete(const char *name);

#endif /* FSI_H */
