/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef DATE_H
#define DATE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file date.h
 * @brief System time: UTC, as seconds since 1970, in step with the kernel's clock.
 *
 * It starts at 0 and counts from then, so until someone sets it, it reads as
 * the time since reset. Nothing keeps it across a reset. It is set by the
 * `date` command, or by writing the node `date`, which is how a node on a
 * network gets it:
 *
 *     date                            2026-11-01 12:00:00
 *     date -s "2026-11-01 12:00:00"
 *     date +%s                        1793534400
 *     echo 2026-11-01 12:00:00 > date
 *     uptime                          up 3 days 02:14:07
 */

/** @brief Seconds since 1970, UTC. */
uint32_t date_now(void);

/** @brief Sets the time. */
void date_set(uint32_t seconds);

/** @brief Whether the time was set since reset. */
bool date_is_set(void);

/** @brief Seconds since reset. */
uint32_t date_uptime(void);

/**
 * @brief Writes "YYYY-MM-DD hh:mm:ss" and its 0 to @p text, 20 bytes.
 */
void date_format(uint32_t seconds, char *text);

/**
 * @brief Reads "YYYY-MM-DD hh:mm:ss" (the time may be left out) or "@SECONDS".
 * @return 0, or -1 when @p text is neither.
 */
int date_parse(const char *text, uint32_t *seconds);

/** @brief Creates the node `date`. */
int date_initialize(void);

/** @brief The `date` and `uptime` commands. */
int date_execute(void *args, int argc, char *argv[]);
int date_uptime_execute(void *args, int argc, char *argv[]);

#endif /* DATE_H */
