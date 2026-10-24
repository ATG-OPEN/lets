/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef LS_H
#define LS_H

/**
 * @brief The `ls [-a] [-l]` command: lists the nodes.
 *
 * Plain: the names on one line. -l: one node per line, its handle and what
 * it can do (o open, c close, r read, w write, s lseek, x execute, h hidden):
 * @code
 *  3  --rw-x-  ttyS1
 *  9  -----xh  ls
 * @endcode
 * -a also lists the hidden nodes (commands, stdin, stdout, stderr); options
 * combine (-la, -al).
 *
 * @return The number of nodes listed, or -1.
 */
int ls(void *args, int argc, char *argv[]);

#endif /* LS_H */
