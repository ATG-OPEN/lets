/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* byte8 packet framing; the format is described in byte8.h. */

#include "byte8.h"

#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>

/* one byte into a CRC-16/MODBUS */
static uint16_t byte8_crc16_update(uint16_t crc, uint8_t data)
{
	crc ^= (uint16_t)data;

	for (int i = 0; i < 8; i++) {
		if (crc & 1) {
			crc = (crc >> 1) ^ 0xA001;
		} else {
			crc >>= 1;
		}
	}

	return crc;
}

/* the CRC as sent: a 0xFF byte would look like a packet start, so it becomes 0xFE */
static uint16_t byte8_crc16_result(void *buffer, size_t length)
{
	uint8_t *buf = (uint8_t *)buffer;
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < length; i++) {
		crc = byte8_crc16_update(crc, buf[i]);
	}

	uint8_t lo = (uint8_t)(crc & 0xFF);
	uint8_t hi = (uint8_t)(crc >> 8);

	if (lo == BYTE8_HEADER_SYMBOL) {
		lo = 0xFE;
	}

	if (hi == 0xFF) {
		hi = 0xFE;
	}

	return (uint16_t)((uint16_t)hi << 8 | lo);
}

/*
 * Feeds received bytes through the receiver state machine. Data bytes go to
 * buf_data, with the replaced 0xFF bytes restored. Returns 0 when a packet is
 * complete, or a negative byte8_error; *used gets the number of bytes consumed
 * when a packet ended, good or bad.
 */
static int byte8_process(uint8_t *buf_raw, unsigned int raw_size, uint8_t *buf_data,
			 byte8_t *session, int *used)
{
	for (int i = 0; i < (int)raw_size; i++) {
		/* 0xFF always starts a new packet, whatever came before */
		if (buf_raw[i] == BYTE8_HEADER_SYMBOL) {
			session->rx_status = BYTE8_WAITING_SIZE;
			session->rx_pointer = 0;

			continue;
		}

		switch (session->rx_status) {
		case BYTE8_WAITING_HEADER:
			break;
		case BYTE8_WAITING_SIZE:
			session->rx_size = (buf_raw[i] > BYTE8_MAX_DATA_SIZE) ? BYTE8_MAX_DATA_SIZE
									      : (int)buf_raw[i];
			session->rx_status = BYTE8_WAITING_NHI;
			break;

		case BYTE8_WAITING_NHI:
			session->rx_last_header_symbol = (int)buf_raw[i];
			session->rx_pointer = 0;
			session->rx_status = BYTE8_WAITING_DATA;
			break;

		case BYTE8_WAITING_DATA:
			if (session->rx_pointer == session->rx_last_header_symbol) {
				/*
				 * A replaced 0xFF: it holds the index of the next one.
				 * Read the index first: byte8_read() decodes in place,
				 * and when a packet spans two reads, buf_data[rx_pointer]
				 * is the same byte as buf_raw[i].
				 */
				session->rx_last_header_symbol = (int)buf_raw[i];
				buf_data[session->rx_pointer] = BYTE8_HEADER_SYMBOL;
			} else {
				buf_data[session->rx_pointer] = (int)buf_raw[i];
			}

			session->rx_pointer++;

			if (session->rx_pointer < session->rx_size) {
				break;
			}

			if (session->mode == BYTE8_STANDARD_MODE) {
				session->rx_status = BYTE8_WAITING_HEADER;
				session->rx_pointer = 0;
				*used = i + 1;
				return 0;
			} else if (session->mode == BYTE8_CRC16_MODE) {
				session->rx_status = BYTE8_WAITING_CRCL;
			}

			break;

		case BYTE8_WAITING_CRCL:
			session->crc = (uint16_t)buf_raw[i] & 0x00FF;
			session->rx_status = BYTE8_WAITING_CRCH;
			break;

		case BYTE8_WAITING_CRCH:
			session->crc |= ((uint16_t)buf_raw[i] << 8) & 0xFF00;
			session->rx_status = BYTE8_WAITING_HEADER;
			session->rx_pointer = 0;
			*used = i + 1;
			return (session->crc == byte8_crc16_result(buf_data, session->rx_size))
				       ? 0
				       : BYTE8_ERROR_CRC16;
			break;
		default:
			break;
		}
	}
	return BYTE8_ERROR_NO_PACKET;
}

int byte8_feed(const void *raw, unsigned int n, void *buf, unsigned int size, byte8_t *session,
	       unsigned int *used)
{
	int end = (int)n;

	size = (size > BYTE8_MAX_DATA_SIZE) ? BYTE8_MAX_DATA_SIZE : size;

	/* the packet is put together in pr_buffer; byte8_read() passes pr_buffer as raw too */
	int k = byte8_process((uint8_t *)raw, n, session->pr_buffer, session, &end);

	*used = (unsigned int)end;

	if (k < 0) {
		return k;
	}

	/* rx_size comes from the wire: drop packets larger than the caller's buffer */
	if (session->rx_size < 0 || (unsigned int)session->rx_size > size) {
		return BYTE8_ERROR_NO_PACKET;
	}

	memcpy(buf, session->pr_buffer, session->rx_size);

	return session->rx_size;
}

int byte8_read(int fd, void *buf, unsigned int size, byte8_t *session)
{
	/* read more only when the bytes of the last read are used up */
	if (session->pr_size < 1) {
		int n = read(fd, session->pr_buffer + session->rx_pointer,
			     BYTE8_MAX_PACKET_SIZE - session->rx_pointer);

		/* -1 is nothing to read too: a stream that doesn't wait, or a closed one */
		session->pr_size = (n > 0) ? n : 0;
	}

	/*
	 * The bytes are decoded in place. Where this read's bytes start is kept:
	 * byte8_process() moves rx_pointer, and what follows a packet is counted
	 * from here.
	 */
	int raw = session->rx_pointer;
	unsigned int used;
	int k = byte8_feed(session->pr_buffer + raw, session->pr_size, buf, size, session, &used);

	/* keep the bytes after this packet for the next call, also after a bad one */
	session->pr_size -= (int)used;

	if (session->pr_size > 0) {
		memmove(session->pr_buffer, session->pr_buffer + raw + used, session->pr_size);
	}

	return k;
}

int byte8_encode(const void *data, unsigned int size, void *frame, uint8_t mode)
{
	uint8_t *tx = frame;

	size = (size > BYTE8_MAX_DATA_SIZE) ? BYTE8_MAX_DATA_SIZE : size;

	if (size == 0) {
		return 0;
	}

	memcpy(tx + BYTE8_OVERHEAD, data, size);

	tx[0] = BYTE8_HEADER_SYMBOL;
	tx[1] = (uint8_t)size;
	tx[2] = BYTE8_NO_HEADER_INDICATOR;

	/*
	 * Replace each 0xFF in the data and chain them: the previous link
	 * (first tx[2], then the previous replaced byte) gets its index.
	 */
	uint8_t j = 2;

	for (uint8_t i = BYTE8_OVERHEAD; i < size + BYTE8_OVERHEAD; i++) {
		if (tx[i] == BYTE8_HEADER_SYMBOL) {
			tx[i] = BYTE8_NO_HEADER_INDICATOR;
			tx[j] = i - BYTE8_OVERHEAD;

			j = i;
		}
	}

	if (mode != BYTE8_CRC16_MODE) {
		return (int)(BYTE8_OVERHEAD + size);
	}

	/* low byte first, as byte8_process() reads it */
	uint16_t crc = byte8_crc16_result((void *)data, size);

	tx[BYTE8_OVERHEAD + size] = (uint8_t)(crc & 0xFF);
	tx[BYTE8_OVERHEAD + size + 1] = (uint8_t)(crc >> 8);

	return (int)(BYTE8_OVERHEAD + size + BYTE8_CRC16_OVERHEAD);
}

int byte8_write(int fd, void *buf, unsigned int size, byte8_t *session)
{
	uint8_t tx[BYTE8_MAX_FRAME_SIZE];
	int n = byte8_encode(buf, size, tx, session->mode);

	return (n > 0) ? write(fd, tx, (unsigned int)n) : 0;
}
