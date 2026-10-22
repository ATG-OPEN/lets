/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file byte8.h
 * @brief Packet framing over a byte stream (UART, pipe, socket), with CRC16.
 *
 * A serial line carries bytes, not messages. byte8 puts each message in a
 * packet whose start can always be found again, even after lost bytes:
 *
 * @code{.unparsed}
 *   0xFF  size  link  data[size]  [crc16 low  crc16 high]
 * @endcode
 *
 * 0xFF marks the start of a packet and never appears anywhere else. Every
 * 0xFF in the data is replaced on the wire, and the bytes that were replaced
 * are chained together: `link` holds the index of the first one, and each
 * replaced byte holds the index of the next (0xFE ends the chain). The
 * receiver follows the chain and puts the 0xFF bytes back. So the overhead is
 * always 3 bytes (5 with the CRC), whatever the data.
 *
 * The CRC is CRC-16/MODBUS (polynomial 0xA001, start 0xFFFF), with any 0xFF
 * byte of it sent as 0xFE.
 *
 * Example:
 * @code
 * static byte8_t link = { .mode = BYTE8_CRC16_MODE };
 *
 * byte8_write(fd, &msg, sizeof(msg), &link);           // send
 *
 * int n = byte8_read(fd, buf, sizeof(buf), &link);     // poll
 * if (n > 0)
 *         handle(buf, n);                              // one whole packet
 * @endcode
 */

#ifndef BYTE8_H
#define BYTE8_H

#include <stdint.h>

#define BYTE8_MAX_PACKET_SIZE	  255u	/**< Receive buffer size. */
#define BYTE8_MAX_DATA_SIZE	  250u	/**< Most data bytes per packet. */
#define BYTE8_OVERHEAD		  3u	/**< Start, size and link bytes. */
#define BYTE8_CRC16_OVERHEAD	  2u	/**< CRC bytes, in CRC16 mode. */
#define BYTE8_HEADER_SYMBOL	  0xFFu /**< Start of a packet. */
#define BYTE8_NO_HEADER_INDICATOR 0xFEu /**< End of the 0xFF chain. */

/** Negative results of byte8_read(). */
enum byte8_error {
	BYTE8_ERROR_NO_PACKET = -1, /**< No whole packet yet. */
	BYTE8_ERROR_CRC16 = -2	    /**< A packet arrived with a wrong CRC. */
};

/** Whether packets carry a CRC. Both sides must use the same mode. */
enum byte8_mode {
	BYTE8_STANDARD_MODE = 0, /**< No CRC. */
	BYTE8_CRC16_MODE = 1	 /**< CRC16 after the data. */
};

/** Receiver state: the part of a packet expected next. */
enum byte8_rx_status {
	BYTE8_WAITING_HEADER = 0, /**< The 0xFF start byte. */
	BYTE8_WAITING_SIZE = 1,	  /**< The size byte. */
	BYTE8_WAITING_NHI = 2,	  /**< The link byte (first replaced 0xFF). */
	BYTE8_WAITING_DATA = 3,	  /**< Data bytes. */
	BYTE8_WAITING_CRCL = 4,	  /**< CRC low byte. */
	BYTE8_WAITING_CRCH = 5,	  /**< CRC high byte. */
};

/**
 * One link (one stream). Zero it and set @c mode before the first call;
 * keep it for the life of the link.
 */
struct byte8_session {
	uint8_t mode;	   /**< BYTE8_STANDARD_MODE or BYTE8_CRC16_MODE. */
	uint16_t crc;	   /**< CRC of the packet being received. */
	uint8_t rx_status; /**< enum byte8_rx_status. */

	int rx_last_header_symbol; /**< Index of the next replaced 0xFF. */
	int rx_pointer;		   /**< Data bytes received so far. */
	int rx_size;		   /**< Data bytes in this packet. */

	int pr_size;				  /**< Bytes read but not processed. */
	uint8_t pr_buffer[BYTE8_MAX_PACKET_SIZE]; /**< Bytes from read(). */
};

/** The link state type used by the functions. */
typedef struct byte8_session byte8_t;

/**
 * @brief Reads from @p fd and returns the next whole packet, if any.
 *
 * Call it again and again (from a task): each call reads what has arrived,
 * keeps partial packets for the next call, and returns at most one packet.
 *
 * @param fd      The stream.
 * @param buf     Receives the packet's data.
 * @param size    Size of @p buf; a bigger packet is dropped.
 * @param session The link state.
 * @return Data bytes in @p buf, BYTE8_ERROR_NO_PACKET or BYTE8_ERROR_CRC16.
 */
int byte8_read(int fd, void *buf, unsigned int size, byte8_t *session);

/**
 * @brief Sends @p size bytes as one packet (cut to BYTE8_MAX_DATA_SIZE).
 * @return The result of write(): the bytes written including the overhead.
 */
int byte8_write(int fd, void *buf, unsigned int size, byte8_t *session);

#endif /* BYTE8_H */
