/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* byte8 on a stream that has nothing to give: read() returning 0 or -1. */

#include "../byte8.h"
#include "test.h"

#include <string.h>

/* the stream: bytes to read, then @c test_read_empty as read()'s result */
static uint8_t test_stream[512];
static size_t test_stream_size;
static size_t test_stream_pos;
static ssize_t test_read_empty;

ssize_t test_read(int fd, void *buffer, size_t size)
{
	size_t left = test_stream_size - test_stream_pos;

	(void)fd;

	if (left == 0) {
		return test_read_empty;
	}

	size = (size > left) ? left : size;
	memcpy(buffer, test_stream + test_stream_pos, size);
	test_stream_pos += size;
	return (ssize_t)size;
}

ssize_t test_write(int fd, const void *buffer, size_t size)
{
	(void)fd;

	memcpy(test_stream + test_stream_size, buffer, size);
	test_stream_size += size;
	return (ssize_t)size;
}

static void test_empty_stream(ssize_t empty)
{
	byte8_t rx, tx;
	uint8_t data[] = {1, 0xFF, 3, 0xFF};
	uint8_t out[BYTE8_MAX_DATA_SIZE];

	memset(&rx, 0, sizeof(rx));
	memset(&tx, 0, sizeof(tx));
	test_stream_size = 0;
	test_stream_pos = 0;
	test_read_empty = empty;

	/* nothing yet */
	CHECK(byte8_read(0, out, sizeof(out), &rx) == BYTE8_ERROR_NO_PACKET);

	/* two packets arrive in one read: nothing is left for the read after the second */
	byte8_write(0, data, sizeof(data), &tx);
	byte8_write(0, data, sizeof(data), &tx);
	CHECK(byte8_read(0, out, sizeof(out), &rx) == (int)sizeof(data));
	CHECK(memcmp(out, data, sizeof(data)) == 0);
	CHECK(byte8_read(0, out, sizeof(out), &rx) == (int)sizeof(data));
	CHECK(byte8_read(0, out, sizeof(out), &rx) == BYTE8_ERROR_NO_PACKET);
	CHECK(byte8_read(0, out, sizeof(out), &rx) == BYTE8_ERROR_NO_PACKET);

	/* half a packet, then nothing, then the rest */
	test_stream_size = 0;
	test_stream_pos = 0;
	byte8_write(0, data, sizeof(data), &tx);
	size_t whole = test_stream_size;
	test_stream_size = 4;
	CHECK(byte8_read(0, out, sizeof(out), &rx) == BYTE8_ERROR_NO_PACKET);
	CHECK(byte8_read(0, out, sizeof(out), &rx) == BYTE8_ERROR_NO_PACKET);
	test_stream_size = whole;
	CHECK(byte8_read(0, out, sizeof(out), &rx) == (int)sizeof(data));
	CHECK(memcmp(out, data, sizeof(data)) == 0);
}

int main(void)
{
	test_empty_stream(0);
	test_empty_stream(-1);

	TEST_END("byte8");
}
