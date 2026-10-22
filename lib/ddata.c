/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "ddata.h"
#include <unistd.h>

int ddata_send(int fd, void *buffer, int size, int *pointer)
{
	if (*pointer >= size) {
		return 1;
	}

	int s = (size - *pointer > DDATA_FRAGMENT_SIZE) ? DDATA_FRAGMENT_SIZE : size - *pointer;

	s = write(fd, (char *)buffer + *pointer, s);

	if (s < 0) {
		return -1;
	}

	*pointer += s;

	if (*pointer >= size) {
		return 1;
	}

	return 0;
}

int ddata_recv(int fd, void *buffer, int size, int *pointer)
{
	if (*pointer >= size) {
		return 1;
	}

	int s = (size - *pointer > DDATA_FRAGMENT_SIZE) ? DDATA_FRAGMENT_SIZE : size - *pointer;

	s = read(fd, (char *)buffer + *pointer, s);

	if (s < 0) {
		return -1;
	}

	*pointer += s;

	if (*pointer >= size) {
		return 1;
	}

	return 0;
}
