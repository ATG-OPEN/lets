/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef SSCRIPT_H
#define SSCRIPT_H

#include <stdint.h>

/**
 * @brief Task that runs a script once: every line is a command for system().
 *
 * The lines are split in place, so the text must be writable.
 *
 * @param args The script text, 0 terminated.
 * @return -1: the task stops after one run.
 */
int sscript(void *args, uint64_t system_time);

#endif /* SSCRIPT_H */
