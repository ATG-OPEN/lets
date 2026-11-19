/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef KILL_H
#define KILL_H

/**
 * @brief The `kill TASK` command: stops a task by name (see `top`).
 *
 * It finds the task's ID among the rows of /proc (as `pkill` on Linux) and calls kill().
 * The task's memory is freed. A task may kill itself; it is not called again.
 *
 * @return 0, or -1 when there is no such task.
 */
int kill_execute(void *args, int argc, char *argv[]);

#endif /* KILL_H */
