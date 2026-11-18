/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file uuid.h
 * @brief A 32-bit ID unique to each chip (PIC24FJ / dsPIC33CK, XC16 only).
 *
 * Microchip programs a unique device ID (UDID, 5 x 24 bits) into every chip.
 * get_unique_id() folds it into 32 bits, for example to give each board its
 * own address or serial number without configuring it.
 *
 * The UDID is at 0x801600 on the PIC24FJ128GL306 and PIC24FJ64GP202 and at
 * 0x801200 on the dsPIC33CK256MP202, as their device files say. Other chips
 * may differ or have none: check yours, a wrong address gives every chip the
 * same ID.
 */

#ifndef UUID_H
#define UUID_H

#include <stdint.h>

/**
 * @brief Returns the chip's unique ID folded into 32 bits.
 *
 * The same chip always gives the same value. Different chips give different
 * values with high probability (it is a hash, not a guarantee). Never 0 or
 * 0xFFFFFFFF.
 */
uint32_t get_unique_id(void);

#endif /* UUID_H */
