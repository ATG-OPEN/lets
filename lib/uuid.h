/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file uuid.h
 * @brief A 32-bit ID unique to each chip (PIC24 / dsPIC33, XC16 only).
 *
 * Microchip programs a unique device ID (UDID, 5 x 24 bits) into every chip.
 * get_unique_id() folds it into 32 bits, for example to give each board its
 * own address or serial number without configuring it.
 *
 * The UDID address (0x801600) is the one of the PIC24FJ / dsPIC33E families;
 * check the programming specification of your device.
 */

#ifndef UUID_H
#define UUID_H

#include <stdint.h>

/**
 * @brief Returns the chip's unique ID folded into 32 bits.
 *
 * The same chip always gives the same value. Different chips give different
 * values with high probability (it is a hash, not a guarantee).
 */
uint32_t get_unique_id(void);

#endif /* UUID_H */
