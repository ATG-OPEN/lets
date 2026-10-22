/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef PORT_HOST_TTY_H
#define PORT_HOST_TTY_H

/*
 * The PC terminal as a kernel node: reads are non-blocking (a task must never block), writes go
 * to the real stdout. Compiled without the prelude, so these are the system's read / write.
 */

void host_tty_setup(void);
int host_tty_read(void *args, void *buffer, unsigned int length);
int host_tty_write(void *args, void *buffer, unsigned int length);

/* microseconds since start (monotonic), for task_initialize() */
unsigned long long host_time_us(void);

/* lets the process sleep while the kernel has nothing to do */
void host_idle(unsigned int us);

#endif
