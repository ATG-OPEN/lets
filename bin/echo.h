/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef ECHO_H
#define ECHO_H

/**
 * @brief The `echo <words> [> <node>]` command: prints its words.
 *
 * With `> node` the line is written to that node instead of stdout.
 */
int echo(void *args, int argc, char *argv[]);

#endif /* ECHO_H */
