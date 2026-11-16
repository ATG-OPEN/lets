/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The few nRF52 registers port/nrf52/nrf52.c uses, as plain memory, for a host test
 * (tests/test_nrf52.c): the test plays the hardware. Names as in the vendor's MDK.
 */
#ifndef TEST_NRF_H
#define TEST_NRF_H

#include <stdint.h>

typedef struct {
	uint32_t TXD, RXD, RTS, CTS;
} fake_psel_t;

typedef struct {
	uint32_t PTR, MAXCNT, AMOUNT;
} fake_dma_t;

typedef struct {
	uint32_t TASKS_STARTRX, TASKS_STOPRX, TASKS_STARTTX, TASKS_STOPTX;
	uint32_t EVENTS_RXDRDY, EVENTS_TXDRDY, EVENTS_ERROR, EVENTS_ENDTX, EVENTS_ENDRX;
	uint32_t ERRORSRC, ENABLE, CONFIG, BAUDRATE, SHORTS, INTENSET, RXD, TXD;
	fake_psel_t PSEL;
	fake_dma_t RXD_DMA, TXD_DMA;
} fake_uart_t;

/* UARTE: the EasyDMA channels are RXD / TXD; the UART's RXD / TXD are registers */
typedef struct {
	uint32_t TASKS_STARTRX, TASKS_STOPRX, TASKS_STARTTX, TASKS_STOPTX;
	uint32_t EVENTS_RXDRDY, EVENTS_TXDRDY, EVENTS_ERROR, EVENTS_ENDTX, EVENTS_ENDRX;
	uint32_t ERRORSRC, ENABLE, CONFIG, BAUDRATE, SHORTS, INTENSET;
	fake_psel_t PSEL;
	fake_dma_t RXD, TXD;
} NRF_UARTE_Type;

typedef struct {
	uint32_t TASKS_STARTRX, TASKS_STOPRX, TASKS_STARTTX, TASKS_STOPTX;
	uint32_t EVENTS_RXDRDY, EVENTS_TXDRDY, EVENTS_ERROR;
	uint32_t ERRORSRC, ENABLE, CONFIG, BAUDRATE, INTENSET, RXD, TXD;
	fake_psel_t PSEL;
} NRF_UART_Type;

typedef struct {
	uint32_t TASKS_START, TASKS_STOP, TASKS_COUNT, TASKS_CLEAR, TASKS_CAPTURE[6];
	uint32_t EVENTS_COMPARE[6];
	uint32_t SHORTS, INTENSET, MODE, BITMODE, PRESCALER, CC[6];
} NRF_TIMER_Type;

typedef struct {
	uint32_t EEP, TEP;
} fake_ppi_ch_t;

typedef struct {
	uint32_t CHENSET;
	fake_ppi_ch_t CH[20];
} NRF_PPI_Type;

typedef struct {
	uint32_t EVENTS_HFCLKSTARTED, TASKS_HFCLKSTART;
} NRF_CLOCK_Type;

typedef struct {
	uint32_t RESETREAS, DCDCEN;
} NRF_POWER_Type;

typedef struct {
	uint32_t OUTSET, OUTCLR, IN, PIN_CNF[32];
} NRF_GPIO_Type;

extern NRF_UARTE_Type fake_uarte0;
extern NRF_TIMER_Type fake_timer2;
extern NRF_PPI_Type fake_ppi;
extern NRF_CLOCK_Type fake_clock;
extern NRF_POWER_Type fake_power;
extern NRF_GPIO_Type fake_p0;
NRF_TIMER_Type *fake_timer1(void); /* its CC[1] moves on at each use: delays end */

#define NRF_UARTE0 (&fake_uarte0)
#define NRF_UART0  ((NRF_UART_Type *)&fake_uarte0)
#define NRF_TIMER1 (fake_timer1())
#define NRF_TIMER2 (&fake_timer2)
#define NRF_PPI	   (&fake_ppi)
#define NRF_CLOCK  (&fake_clock)
#define NRF_POWER  (&fake_power)
#define NRF_P0	   (&fake_p0)

enum { TIMER1_IRQn = 9, UARTE0_UART0_IRQn = 2 };

#define NVIC_SetPriority(irq, p) ((void)(irq), (void)(p))
#define NVIC_EnableIRQ(irq)	 ((void)(irq))
#define NVIC_DisableIRQ(irq)	 ((void)(irq))
#define NVIC_SystemReset()	 ((void)0)

#define TIMER_MODE_MODE_Timer		0u
#define TIMER_MODE_MODE_Counter		1u
#define TIMER_MODE_MODE_LowPowerCounter 2u
#define TIMER_BITMODE_BITMODE_32Bit	3u
#define TIMER_INTENSET_COMPARE0_Msk	(1u << 16)
#define TIMER_SHORTS_COMPARE0_CLEAR_Msk 1u

#define UART_ENABLE_ENABLE_Disabled  0u
#define UART_ENABLE_ENABLE_Enabled   4u
#define UARTE_ENABLE_ENABLE_Disabled 0u
#define UARTE_ENABLE_ENABLE_Enabled  8u
#define UART_INTENSET_RXDRDY_Msk     (1u << 2)
#define UART_INTENSET_TXDRDY_Msk     (1u << 7)
#define UART_INTENSET_ERROR_Msk	     (1u << 9)
#define UARTE_INTENSET_ENDTX_Msk     (1u << 8)
#define UARTE_INTENSET_ERROR_Msk     (1u << 9)
#define UARTE_SHORTS_ENDRX_STARTRX_Msk (1u << 5)

#define GPIO_PIN_CNF_DIR_Pos	   0
#define GPIO_PIN_CNF_DIR_Output	   1u
#define GPIO_PIN_CNF_PULL_Pos	   2
#define GPIO_PIN_CNF_PULL_Disabled 0u
#define GPIO_PIN_CNF_PULL_Pullup   3u

#define UART_BAUDRATE_BAUDRATE_Baud9600	   0x00275000u
#define UART_BAUDRATE_BAUDRATE_Baud19200   0x004EA000u
#define UART_BAUDRATE_BAUDRATE_Baud38400   0x009D5000u
#define UART_BAUDRATE_BAUDRATE_Baud57600   0x00EBF000u
#define UART_BAUDRATE_BAUDRATE_Baud115200  0x01D7E000u
#define UART_BAUDRATE_BAUDRATE_Baud230400  0x03AFB000u
#define UART_BAUDRATE_BAUDRATE_Baud460800  0x075F7000u
#define UART_BAUDRATE_BAUDRATE_Baud921600  0x0EBEDFA4u
#define UART_BAUDRATE_BAUDRATE_Baud1M	   0x10000000u
#define UARTE_BAUDRATE_BAUDRATE_Baud9600   0x00275000u
#define UARTE_BAUDRATE_BAUDRATE_Baud19200  0x004EA000u
#define UARTE_BAUDRATE_BAUDRATE_Baud38400  0x009D0000u
#define UARTE_BAUDRATE_BAUDRATE_Baud57600  0x00EB0000u
#define UARTE_BAUDRATE_BAUDRATE_Baud115200 0x01D60000u
#define UARTE_BAUDRATE_BAUDRATE_Baud230400 0x03B00000u
#define UARTE_BAUDRATE_BAUDRATE_Baud460800 0x07400000u
#define UARTE_BAUDRATE_BAUDRATE_Baud921600 0x0F000000u
#define UARTE_BAUDRATE_BAUDRATE_Baud1M	   0x10000000u

#endif
