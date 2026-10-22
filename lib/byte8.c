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
 * buf_data, with the replaced 0xFF bytes restored. Returns the number of bytes
 * used when a packet is complete, or a negative byte8_error.
 */
static int byte8_process(uint8_t *buf_raw, unsigned int raw_size, uint8_t *buf_data,
			 byte8_t *session)
{
	for (int i = 0; i < raw_size; i++) {
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
				return i + 1;
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
			return (session->crc == byte8_crc16_result(buf_data, session->rx_size))
				       ? i + 1
				       : BYTE8_ERROR_CRC16;
			break;
		default:
			break;
		}
	}
	return BYTE8_ERROR_NO_PACKET;
}

int byte8_read(int fd, void *buf, unsigned int size, byte8_t *session)
{
	size = (size > BYTE8_MAX_DATA_SIZE) ? BYTE8_MAX_DATA_SIZE : size;

	/* read more only when the bytes of the last read are used up */
	if (session->pr_size < 1) {
		session->pr_size = read(fd, session->pr_buffer + session->rx_pointer,
					BYTE8_MAX_PACKET_SIZE - session->rx_pointer);
	}

	int k = byte8_process(session->pr_buffer + session->rx_pointer, session->pr_size,
			      session->pr_buffer, session);

	if (k < 0) {
		session->pr_size = 0;
		return k;
	} else {
		/* rx_size comes from the wire: drop packets larger than the caller's buffer */
		int fits = (session->rx_size >= 0) && ((unsigned int)session->rx_size <= size);

		if (fits) {
			memcpy(buf, session->pr_buffer, session->rx_size);
		}

		/* keep the bytes after this packet for the next call */
		session->pr_size -= k;

		if (session->pr_size > 0) {
			memmove(session->pr_buffer, session->pr_buffer + session->rx_pointer + k,
				session->pr_size);
		}

		return fits ? session->rx_size : BYTE8_ERROR_NO_PACKET;
	}
}

int byte8_write(int fd, void *buf, unsigned int size, byte8_t *session)
{
	if (size > 0) {
		uint8_t tx[BYTE8_MAX_DATA_SIZE + BYTE8_OVERHEAD + BYTE8_CRC16_OVERHEAD];

		size = (size > BYTE8_MAX_DATA_SIZE) ? BYTE8_MAX_DATA_SIZE : size;

		memcpy(tx + BYTE8_OVERHEAD, buf, size);

		tx[0] = BYTE8_HEADER_SYMBOL;
		tx[1] = size;
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

		if (session->mode == BYTE8_STANDARD_MODE) {
			return write(fd, tx, BYTE8_OVERHEAD + size);
		} else if (session->mode == BYTE8_CRC16_MODE) {
			uint16_t tmp_crc = byte8_crc16_result(buf, size);
			memcpy(tx + BYTE8_OVERHEAD + size, &tmp_crc, sizeof(uint16_t));
			return write(fd, tx, BYTE8_OVERHEAD + size + BYTE8_CRC16_OVERHEAD);
		}
	}
	return size;
}
