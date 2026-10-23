/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The shell: a task that reads one command line from stdin and runs it.
 */

#include "../fsi/fsi.h"

#include "../kernel/task.h"

#include "cat.h"
#include "echo.h"
#include "kill.h"
#include "ls.h"
#include "mv.h"
#include "shell.h"
#include "sscript.h"
#include "tmp.h"
#include "top.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SHELL_BUFFER_SIZE 128 /* longest command line */

static char shell_buffer[SHELL_BUFFER_SIZE];

/* the shell task: polls stdin every 10 ms */
int shell_loop(void *args, uint64_t system_time)
{
	char *buf = args;

	int size = read(0, buf, SHELL_BUFFER_SIZE - 1);

	if (size < 1) {
		return 10;
	}

	/* drop the line ending: terminals send \r, \n or both */
	while (size > 0 && (buf[size - 1] == '\n' || buf[size - 1] == '\r')) {
		size--;
	}

	buf[size] = '\0';

	if (size > 0) {
		system(buf);
	}

	return 10;
}

int shell_initialize(void *args)
{
	fsi_register_app("cat", cat);
	fsi_register_app("echo", echo);
	fsi_register_app("kill", kill);
	fsi_register_app("ls", ls);
	fsi_register_app("mv", mv);
	fsi_register_app("top", top);
	fsi_register_app("touch", tmp_file_create);
	fsi_register_app("rm", tmp_file_delete);

	task_create("shell", shell_loop, (void *)shell_buffer);

	if (args == NULL) {
		return 0;
	}

	task_create("startup", sscript, args);

	return 0;
}
