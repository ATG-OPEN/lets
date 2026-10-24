/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef CAT_H
#define CAT_H

/**
 * @brief The `cat [-x] NODE` command: prints what one read of a node returns.
 *
 * Text is printed as it is (a newline is added when it does not end with
 * one); -x prints the bytes in hex. At most 127 bytes.
 *
 * @return The bytes read, or -1.
 */
int cat(void *args, int argc, char *argv[]);

#endif /* CAT_H */
