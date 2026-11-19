/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "reboot.h"
#include "shell.h"

#include "../kernel/task.h" /* task_create(): the wait is a task */
#include "../lib/posix.h"

#include <stdbool.h>

static bool reboot_due; /* the task has waited: the queues have drained */

/* first a sleep for the queues to drain; then what came meanwhile is written too, and the reset */
static int reboot_task(void *arg, uint64_t now)
{
	if (!reboot_due) {
		reboot_due = true;
		return REBOOT_WAIT_MS;
	}

	sync();
	reboot(0);
}

int reboot_execute(void *args, int argc, char *argv[])
{
	sync();

	task_t *t = task_create("reboot", reboot_task, NULL);

	/* no room for the task: no wait either */
	if (t == NULL) {
		reboot_due = true;
		reboot_task(NULL, 0);
	}

	shell_wait(t);

	return 0;
}
