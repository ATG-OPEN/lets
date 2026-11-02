/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef TMP_H
#define TMP_H

#include "../vfs/vfs.h"

#include <stdint.h>

/**
 * @brief Creates a RAM file (126 bytes) named @p name, as it is stored in the table.
 * @return Its node, or -1 when the heap or the node table is full.
 */
int tmp_file_create(const char *name);

/** @brief Bytes stored in @p node, or -1 when it is not a RAM file (for `ls -l`). */
int tmp_file_size(const vfs_node_t *node);

/** @brief Empties @p node when it is a RAM file (`>`). */
void tmp_file_truncate(const vfs_node_t *node);

#endif /* TMP_H */
