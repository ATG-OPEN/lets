/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "sys.h"

#include "../kernel/task.h"
#include "../vfs/vfs.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SYS_LINE_SIZE
#define SYS_LINE_SIZE 80u /* longest command, with its 0 */
#endif

#ifndef SYS_OUT_SIZE
#define SYS_OUT_SIZE 192u /* the output kept */
#endif

static char sys_line[SYS_LINE_SIZE];
static unsigned int sys_line_length;
static char sys_out[SYS_OUT_SIZE];
static unsigned int sys_out_length;
static int sys_output = -1;	/* the handle that is the command's stdout */
static int sys_index;		/* this node */

/* the command's stdout is this node, and what it starts stays on it */
static int sys_run(void *args, uint64_t system_time)
{
	task_streams_t *s = task_streams();

	sys_out_length = 0;
	sys_output = vfs_node_open(sys_index, O_WRONLY);

	s->out = (int8_t)sys_output;
	s->terminal = 0;

	system(sys_line);
	vfs_stdout_flush();

	return -1;
}

static int sys_open(void *args, int flags)
{
	if (flags & O_ACCMODE) {
		sys_line_length = 0;
	}

	return 0;
}

/* on the command's handle its output, on any other the command */
static int sys_write(void *args, void *buffer, unsigned int length)
{
	bool output = (vfs_fd_now() == sys_output);
	unsigned int n = output ? SYS_OUT_SIZE - sys_out_length : SYS_LINE_SIZE - 1u - sys_line_length;

	n = (n > length) ? length : n;
	memcpy(output ? sys_out + sys_out_length : sys_line + sys_line_length, buffer, n);

	if (output) {
		sys_out_length += n;
	} else {
		sys_line_length += n;
	}

	return (int)length;
}

static int sys_read(void *args, void *buffer, unsigned int length)
{
	unsigned int *position = vfs_position();
	unsigned int left = (*position < sys_out_length) ? sys_out_length - *position : 0u;

	length = (length > left) ? left : length;
	memcpy(buffer, sys_out + *position, length);
	*position += length;

	return (int)length;
}

static int sys_lseek(void *args, off_t offset, int whence)
{
	return vfs_position_seek(sys_out_length, offset, whence);
}

static int sys_close(void *args)
{
	if (vfs_fd_now() == sys_output) {
		sys_output = -1;
		return 0;
	}

	unsigned int n = sys_line_length;

	if (n == 0u) {
		return 0;
	}

	sys_line_length = 0;
	sys_line[n] = '\0';

	/* one line */
	sys_line[strcspn(sys_line, "\r\n")] = '\0';

	if (sys_line[0] != '\0' && task_create("sys", sys_run, NULL) == NULL) {
		return -1;
	}

	return 0;
}

static const vfs_handler_t sys_node = {
	.open_handler = sys_open,
	.read_handler = sys_read,
	.write_handler = sys_write,
	.lseek_handler = sys_lseek,
	.close_handler = sys_close,
};

int sys_initialize(void)
{
	sys_index = vfs_node_create("sys", false, NULL, &sys_node);

	return (sys_index < 0) ? -1 : 0;
}
