/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef CAT_H
#define CAT_H

/**
 * @brief The `cat [-x] <node>` command: prints what a node returns on read.
 *
 * -x prints the bytes in hex. Returns the bytes read, or -1 on error.
 */
int cat(void *args, int argc, char *argv[]);

#endif /* CAT_H */
