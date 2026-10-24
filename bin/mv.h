/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef MV_H
#define MV_H

/**
 * @brief The `mv NODE NEWNAME` command: renames a node.
 *
 * The new name must be free and fit (FSI_NODE_NAME_MAX_LENGTH - 1
 * characters). stdin, stdout and stderr keep their names.
 *
 * @return 0, or -1.
 */
int mv(void *args, int argc, char *argv[]);

#endif /* MV_H */
