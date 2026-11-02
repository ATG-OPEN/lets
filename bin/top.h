/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef TOP_H
#define TOP_H

/**
 * @brief The `top` command: the system and every task, with memory and timing.
 *
 * @code{.unparsed}
 *   up 0d 00:00:08  tasks 5  loop 41us (24390/s)  reset MCLR pin
 *   heap 792 of 1700 B used (46%)  stack 638 of 1018 B (62%)
 *    ID NAME            STATE   RAM    LAST     MAX  PERIOD   CPU
 *     0 shell           sleep    0B   287us   287us    10ms  2.8%
 *     1 top             run    100B  1.18ms  1.60ms  5.45ms 23.6%
 *     2 link_rx         sleep    0B    45us   171us  5.96ms  0.9%
 *     4 link_pg         sleep  290B    20us    60us   999ms  0.0%
 * @endcode
 *
 * Per task: the slot, its name, state (run, sleep, stop), the heap it owns, how
 * long its last call took, the longest call so far (what the others had to
 * wait), the time between its last two calls, and its share of the time.
 *
 * A task of its own writes the lines, one at a time: on a DRN port, whose queue
 * takes only so many packets, a line waits until there is room, so none is
 * lost. The shell waits for it before the next prompt (Ctrl-C stops it).
 * `top mem` prints the heap blocks instead (see alloc_info()).
 */
int top(void *args, int argc, char *argv[]);

#endif /* TOP_H */
