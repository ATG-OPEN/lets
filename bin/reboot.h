/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef REBOOT_H
#define REBOOT_H

#ifndef REBOOT_WAIT_MS
#define REBOOT_WAIT_MS 200 /**< Time the queues of the ttys get to drain before the reset. */
#endif

/**
 * @brief The `reboot` command: resets the board without losing what was written.
 *
 * At once it passes on what stdout holds and writes back what the block
 * devices hold back (vfs_sync(): a /boot kept in RAM and written to flash a
 * moment later). Then, as a task, it waits REBOOT_WAIT_MS, so what the ttys
 * still have to send (the consoles, a link) goes out, syncs once more and
 * calls reboot_reset(). The shell that typed it waits for it.
 *
 * @return 0.
 */
int reboot_execute(void *args, int argc, char *argv[]);

/**
 * @brief The reset itself: `reset` on XC16; elsewhere exit(0), which resets the chip
 * through the port's _exit() (a PC: the process ends). Weak: a board can give its own.
 */
void reboot_reset(void);

#endif /* REBOOT_H */
