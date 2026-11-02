/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The kernel on a PC: a shell on your terminal. Type `ls`, `top`, `echo hello`, `touch f`,
 * `cat f`, `kill <task>`; Ctrl-C quits.
 *
 * Same boot sequence as on an MCU (see docs/kernel.md), with the terminal as the board's only
 * device node.
 */
#include "../../kernel/task.h"
#include "../../kernel/alloc.h"
#include "../../bin/shell.h"
#include "../../vfs/vfs.h"
#include "../../port/host/host_tty.h"

ALLOC_INIT_HEAP(16384);

static uint64_t system_time_get(void)
{
	return (uint64_t)host_time_us();
}

static vfs_handler_t tty = {
	.read_handler = host_tty_read,
	.write_handler = host_tty_write,
};

/* the kernel loop never waits; on a PC let the process sleep a little each round */
static int idle(void *args, uint64_t system_time)
{
	(void)args;
	(void)system_time;
	host_idle(1000);
	return 0;
}

int main(void)
{
	host_tty_setup();

	task_initialize(system_time_get);
	vfs_initialize();
	vfs_node_create("tty", false, NULL, &tty); /* the board's only device */
	shell_initialize();
	vfs_bin_initialize();
	task_create("idle", idle, NULL);

	/* the startup, as a board's /boot/rc would do it */
	system("std tty");
	system("echo kernel running on the host - try: ls, top, echo hi, touch f, cat f");

	tasks_start(); /* never returns */
	return 0;
}
