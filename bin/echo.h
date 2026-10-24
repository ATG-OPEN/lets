/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef ECHO_H
#define ECHO_H

/**
 * @brief The `echo WORDS [> NODE]` command: prints its words.
 *
 * The words go out one space apart, with a newline, to stdout, or with
 * `> NODE` (or `>> NODE`) to that node: `echo 12.5 > dac1`.
 *
 * @return 0, or -1 when the node cannot be opened or written.
 */
int echo(void *args, int argc, char *argv[]);

#endif /* ECHO_H */
