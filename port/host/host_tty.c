/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#define _POSIX_C_SOURCE 200809L
#include "host_tty.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

void host_tty_setup(void)
{
	int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

int host_tty_read(void *args, void *buffer, unsigned int length)
{
	(void)args;
	ssize_t n = read(STDIN_FILENO, buffer, length);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return 0;
	}
	return (int)n;
}

int host_tty_write(void *args, void *buffer, unsigned int length)
{
	(void)args;
	return (int)write(STDOUT_FILENO, buffer, length);
}

unsigned long long host_time_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000ull + (unsigned long long)t.tv_nsec / 1000u;
}

void host_idle(unsigned int us)
{
	struct timespec t = {0, (long)us * 1000L};
	nanosleep(&t, NULL);
}
