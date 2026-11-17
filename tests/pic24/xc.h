/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The few PIC24 registers opt/tty.c uses, as plain memory, for a host test
 * (tests/test_tty.c): the test plays the UARTs. The CPU priority (IPL) is a
 * recursive lock: raised, it holds off the thread that plays the interrupts.
 */
#ifndef TEST_XC_H
#define TEST_XC_H

#include <stdint.h>

extern volatile uint16_t fake_u1[5], fake_u2[5]; /* UxMODE, UxSTA, UxTXREG, UxRXREG, UxBRG */
extern volatile uint16_t LATF;
extern volatile unsigned int _U1TXIF, _U1TXIE, _U1RXIF, _U1RXIE;
extern volatile unsigned int _U2TXIF, _U2TXIE, _U2RXIF, _U2RXIE;

#define U1MODE fake_u1[0]
#define U2MODE fake_u2[0]

unsigned int fake_ipl_raise(unsigned int ipl);
void fake_ipl_restore(unsigned int ipl);

/* as in the XC16 device headers */
#define SET_AND_SAVE_CPU_IPL(save_to, ipl) { save_to = fake_ipl_raise(ipl); } (void)0
#define RESTORE_CPU_IPL(saved_to) fake_ipl_restore(saved_to)

/* the interrupt attributes of the vendor compiler */
#define interrupt   unused
#define no_auto_psv unused

#endif /* TEST_XC_H */
