/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file spi.h
 * @brief The SPI bus as the memory drivers see it: one chip select and a byte exchange.
 *
 * The board provides both, for example with MCC:
 * @code
 * static void ee_select(bool on)
 * {
 *         if (on)
 *                 EE_CS_SetLow();
 *         else
 *                 EE_CS_SetHigh();
 * }
 *
 * static const vfs_spi_t ee_spi = {ee_select, SPI1_Exchange8bit};
 * @endcode
 *
 * Chips on the same bus share the exchange function and differ by select.
 */

#ifndef VFS_SPI_H
#define VFS_SPI_H

#include <stdint.h>
#include <stdbool.h>

/** One chip on an SPI bus (mode 0 or 3, MSB first). */
typedef struct {
	void (*select)(bool on);	   /**< true: chip select low (active). */
	uint8_t (*exchange)(uint8_t byte); /**< Sends a byte, returns the byte received. */
} vfs_spi_t;

#endif /* VFS_SPI_H */
