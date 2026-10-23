/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef LS_H
#define LS_H

/**
 * @brief The `ls [-a] [-l]` command: lists the nodes.
 *
 * -a also lists hidden nodes (commands), -l prints one node per line with the
 * handlers it has. Returns the number of nodes printed with -l.
 */
int ls(void *args, int argc, char *argv[]);

#endif /* LS_H */
