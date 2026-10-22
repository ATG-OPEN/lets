/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef TOP_H
#define TOP_H

/**
 * @brief The `top` command: prints the tasks with their memory and timing.
 *
 * First line: task count, free heap, heap size, last loop time (ms) and the
 * loop rate. Then one line per task: index, name, owned memory, duration of
 * its last call and the time between its calls. `top mem` prints the heap
 * summary instead (see alloc_info()).
 */
int top(void *args, int argc, char *argv[]);

#endif /* TOP_H */
