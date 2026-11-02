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

/*
 * On a shared bus packets arrive back to back, so one read() often ends a packet
 * and starts the next one.
 */
static void test_spanning_packets(void)
{
	byte8_t rx, tx;
	uint8_t a[20], b[20];
	uint8_t out[BYTE8_MAX_DATA_SIZE];

	memset(&rx, 0, sizeof(rx));
	memset(&tx, 0, sizeof(tx));
	memset(a, 0x11, sizeof(a));
	memset(b, 0x22, sizeof(b));
	test_stream_size = 0;
	test_stream_pos = 0;
	test_read_empty = 0;

	/* a arrives in two reads; the second read also holds all of b */
	byte8_write(0, a, sizeof(a), &tx);
	byte8_write(0, b, sizeof(b), &tx);
	size_t whole = test_stream_size;

	test_stream_size = 10;
	CHECK(byte8_read(0, out, sizeof(out), &rx) == BYTE8_ERROR_NO_PACKET);
	test_stream_size = whole;
	CHECK(byte8_read(0, out, sizeof(out), &rx) == (int)sizeof(a));
	CHECK(memcmp(out, a, sizeof(a)) == 0);
	CHECK(byte8_read(0, out, sizeof(out), &rx) == (int)sizeof(b));
	CHECK(memcmp(out, b, sizeof(b)) == 0);
}

/* a packet with a wrong CRC costs that packet, not the good one behind it */
static void test_bad_crc_keeps_next(void)
{
	byte8_t rx, tx;
	uint8_t a[8], b[8];
	uint8_t out[BYTE8_MAX_DATA_SIZE];

	memset(&rx, 0, sizeof(rx));
	memset(&tx, 0, sizeof(tx));
	memset(a, 0x33, sizeof(a));
	memset(b, 0x44, sizeof(b));
	rx.mode = BYTE8_CRC16_MODE;
	tx.mode = BYTE8_CRC16_MODE;
	test_stream_size = 0;
	test_stream_pos = 0;
	test_read_empty = 0;

	byte8_write(0, a, sizeof(a), &tx);
	test_stream[5] ^= 0x01;
	byte8_write(0, b, sizeof(b), &tx);

	CHECK(byte8_read(0, out, sizeof(out), &rx) == BYTE8_ERROR_CRC16);
	CHECK(byte8_read(0, out, sizeof(out), &rx) == (int)sizeof(b));
	CHECK(memcmp(out, b, sizeof(b)) == 0);
}

/*
 * byte8_feed() on bytes already in memory, given a few at a time: packets with
 * 0xFF in them, in CRC mode, back to back, one spanning many calls. Then the
 * same bytes through byte8_read() must give the same packets.
 */
static void test_feed_and_encode(void)
{
	byte8_t rx;
	uint8_t a[30], b[3] = {0xFF, 0x00, 0xFF};
	uint8_t wire[2 * BYTE8_MAX_FRAME_SIZE];
	uint8_t out[BYTE8_MAX_DATA_SIZE];
	const uint8_t *packets[2] = {a, b};
	const int sizes[2] = {sizeof(a), sizeof(b)};
	int got = 0;

	for (unsigned int i = 0; i < sizeof(a); i++) {
		a[i] = (i % 4 == 0) ? 0xFF : (uint8_t)i;
	}

	CHECK(byte8_encode(a, 0, wire, BYTE8_CRC16_MODE) == 0);

	int n = byte8_encode(a, sizeof(a), wire, BYTE8_CRC16_MODE);
	CHECK(n == (int)(sizeof(a) + BYTE8_OVERHEAD + BYTE8_CRC16_OVERHEAD));
	n += byte8_encode(b, sizeof(b), wire + n, BYTE8_CRC16_MODE);

	/* 0xFF only starts a packet */
	for (int i = 0; i < n; i++) {
		CHECK(wire[i] != 0xFF || i == 0 || i == (int)(sizeof(a) + 5));
	}

	memset(&rx, 0, sizeof(rx));
	rx.mode = BYTE8_CRC16_MODE;

	for (int pos = 0; pos < n;) {
		int chunk = (n - pos < 7) ? n - pos : 7;
		unsigned int used = 0;
		int k = byte8_feed(wire + pos, (unsigned int)chunk, out, sizeof(out), &rx, &used);

		CHECK(used > 0 && used <= (unsigned int)chunk);
		pos += (int)used;

		if (k > 0) {
			CHECK(got < 2 && k == sizes[got] && memcmp(out, packets[got], k) == 0);
			got++;
		} else {
			CHECK(k == BYTE8_ERROR_NO_PACKET);
		}
	}

	CHECK(got == 2);

	/* byte8_write() sends what byte8_encode() makes */
	byte8_t tx;

	memset(&tx, 0, sizeof(tx));
	tx.mode = BYTE8_CRC16_MODE;
	test_stream_size = 0;
	test_stream_pos = 0;
	test_read_empty = 0;
	byte8_write(0, a, sizeof(a), &tx);
	CHECK(test_stream_size == sizeof(a) + 5 && memcmp(test_stream, wire, test_stream_size) == 0);
}

int main(void)
{
	test_empty_stream(0);
	test_empty_stream(-1);
	test_spanning_packets();
	test_bad_crc_keeps_next();
	test_feed_and_encode();

	TEST_END("byte8");
}
