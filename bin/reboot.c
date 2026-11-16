/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "reboot.h"
#include "shell.h"

#include "../kernel/task.h"
#include "../vfs/vfs.h"

#include <stdlib.h>

void __attribute__((weak)) reboot_reset(void)
{
#if defined(__XC16__)
	__asm__ volatile("reset");
#else
	exit(0);
#endif
}

/* the queues have drained: what came meanwhile is written too, then the reset */
static int reboot_task(void *arg, uint64_t now)
{
	vfs_sync();
	reboot_reset();

	return -1;
}

int reboot_execute(void *args, int argc, char *argv[])
{
	vfs_sync();

	task_t *t = task_create("reboot", reboot_task, NULL);

	/* no room for the task: no wait either */
	if (t == NULL) {
		reboot_task(NULL, 0);
		return 0;
	}

	t->state = TASK_SLEEPING;
	t->wake_tick = (uint32_t)task_get_system_time() + REBOOT_WAIT_MS * 1000ul;
	shell_wait(t);

	return 0;
}
