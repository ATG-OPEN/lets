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
#include "../../kernel/bin/shell.h"
#include "../../fsi/fsi.h"
#include "../../port/host/host_tty.h"

ALLOC_INIT_HEAP(16384);

static uint64_t system_time_get(void)
{
	return (uint64_t)host_time_us();
}

static fsi_handler_t tty = {
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

static char startup[] = "std tty\n"
			"echo kernel running on the host - try: ls, top, echo hi, touch f, cat f\n";

int main(void)
{
	host_tty_setup();

	task_initialize(system_time_get);
	fsi_initialize();
	fsi_node_create("tty", false, NULL, &tty); /* the board's only device */
	shell_initialize((void *)startup);
	task_create("idle", idle, NULL);

	tasks_start(); /* never returns */
	return 0;
}
