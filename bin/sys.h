/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef SYS_H
#define SYS_H

/**
 * @file sys.h
 * @brief The node `sys`: a command line as a file.
 *
 * What is written to it runs as a system() call when the file is closed, and
 * what that command prints is then read from it, from the start, until the
 * next command:
 *
 *     echo df > sys
 *     cat sys
 *
 * The command runs after close() returned, as a task of its own, so whoever
 * wrote it has its answer first: `reboot` works over a network. It takes one
 * line, up to SYS_LINE_SIZE - 1 characters; output past SYS_OUT_SIZE is dropped,
 * and a script started with it prints to the standard streams once it runs.
 */

/** @brief Creates the node (call after vfs_initialize()). @return 0 or -1. */
int sys_initialize(void);

#endif /* SYS_H */
