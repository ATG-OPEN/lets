/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef TMP_H
#define TMP_H

#include "../fsi/fsi.h"

#include <stdint.h>

/** @brief The `touch <name>` command: creates a RAM file (126 bytes). */
int tmp_file_create(void *args, int argc, char *argv[]);

/** @brief The `rm <name>` command: deletes a RAM file. */
int tmp_file_delete(void *args, int argc, char *argv[]);

/** @brief Write handler of a RAM file: appends; -1 when it does not fit. */
int tmp_write(void *args, void *buffer, unsigned int length);

/** @brief Read handler of a RAM file: reads from its start, every time. */
int tmp_read(void *args, void *buffer, unsigned int length);

/** @brief Bytes stored in @p node, or -1 when it is not a RAM file (for `ls -l`). */
int tmp_file_size(const fsi_node_t *node);

#endif /* TMP_H */
