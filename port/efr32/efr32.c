/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * LETS on the EFR32 Series 2: see efr32.h. Registers through the CMSIS device header, no emlib.
 */
#include "efr32.h"

#include "../../vfs/vfs.h"

#include <em_device.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- clock ---------------------------------------------------------------------------- */

static volatile uint32_t overflows; /* TIMER0 wraps every 71.6 minutes */
static uint32_t reset_reason;

void TIMER0_IRQHandler(void)
{
	if (TIMER0->IF & TIMER_IF_OF) {
		TIMER0->IF_CLR = TIMER_IF_OF;
		overflows++;
	}
}

uint32_t efr32_hz = 20000000u; /* FSRCO after reset */

/* the crystal as emlib's CMU_HFXOInit (CMU_HFXOINIT_DEFAULT) + CMU_HFXOCTuneSet: XO gets
 * CTUNE + 40 on xG23 (Series 2 config 3: the crystal sharing buffer loads XI and XO unevenly).
 * false: it did not start (~1 ms normally). */
static bool hfxo_start(void)
{
	uint32_t ctune_xo = EFR32_HFXO_CTUNE + 40u;

	if (ctune_xo > 255u) {
		ctune_xo = 255u;
	}

	CMU->CLKEN0_SET = CMU_CLKEN0_HFXO0;
	HFXO0->LOCK = HFXO_LOCK_LOCKKEY_UNLOCK;
	HFXO0->CTRL_SET = HFXO_CTRL_DISONDEMAND;
	HFXO0->CTRL_CLR = HFXO_CTRL_FORCEEN;
	while (HFXO0->STATUS & (HFXO_STATUS_ENS | HFXO_STATUS_SYNCBUSY)) {
	}

	HFXO0->XTALCFG = (_HFXO_XTALCFG_TIMEOUTCBLSB_T416US << _HFXO_XTALCFG_TIMEOUTCBLSB_SHIFT) |
			 (_HFXO_XTALCFG_TIMEOUTSTEADY_T833US << _HFXO_XTALCFG_TIMEOUTSTEADY_SHIFT) |
			 (32u << _HFXO_XTALCFG_COREBIASSTARTUP_SHIFT) |
			 (32u << _HFXO_XTALCFG_COREBIASSTARTUPI_SHIFT);
	HFXO0->XTALCTRL = (HFXO0->XTALCTRL & _HFXO_XTALCTRL_SKIPCOREBIASOPT_MASK) |
			  (_HFXO_XTALCTRL_CTUNEFIXANA_BOTH << _HFXO_XTALCTRL_CTUNEFIXANA_SHIFT) |
			  (ctune_xo << _HFXO_XTALCTRL_CTUNEXOANA_SHIFT) |
			  (EFR32_HFXO_CTUNE << _HFXO_XTALCTRL_CTUNEXIANA_SHIFT) |
			  (60u << _HFXO_XTALCTRL_COREBIASANA_SHIFT);
	/* emlib does this on config 3 (an undocumented register at +0x38, bits 11:10 = 2) */
	{
		volatile uint32_t *r = (volatile uint32_t *)(HFXO0_BASE + 0x38u);

		*r = (*r & ~0xC00u) | (2u << 10);
	}
	HFXO0->CFG = HFXO0->CFG & ~(_HFXO_CFG_SQBUFSCHTRGANA_MASK | _HFXO_CFG_ENXIDCBIASANA_MASK |
				    _HFXO_CFG_MODE_MASK); /* crystal */
	HFXO0->CTRL = (HFXO0->CTRL & ~(_HFXO_CTRL_FORCEXO2GNDANA_MASK | _HFXO_CTRL_FORCEXI2GNDANA_MASK |
				       _HFXO_CTRL_DISONDEMAND_MASK | _HFXO_CTRL_EM23ONDEMAND_MASK |
				       _HFXO_CTRL_FORCEEN_MASK)) |
		      HFXO_CTRL_FORCEEN;

	const uint32_t ready = HFXO_STATUS_RDY | HFXO_STATUS_COREBIASOPTRDY | HFXO_STATUS_ENS;

	/* FSRCO 20 MHz, a few cycles a loop: ~100 ms at most */
	for (uint32_t n = 0; (HFXO0->STATUS & ready) != ready; n++) {
		if (n > 500000u) {
			return false;
		}
	}

	/* later locks: the short steady time, no core bias search; it stays on (FORCEEN) */
	HFXO0->CTRL_SET = HFXO_CTRL_DISONDEMAND;
	HFXO0->XTALCFG = (HFXO0->XTALCFG & ~_HFXO_XTALCFG_TIMEOUTSTEADY_MASK) |
			 (_HFXO_XTALCFG_TIMEOUTSTEADY_T83US << _HFXO_XTALCFG_TIMEOUTSTEADY_SHIFT);
	HFXO0->XTALCTRL_SET = HFXO_XTALCTRL_SKIPCOREBIASOPT;
	HFXO0->CTRL_CLR = HFXO_CTRL_DISONDEMAND;
	return true;
}

void efr32_initialize(void)
{
	reset_reason = EMU->RSTCAUSE;
	EMU->CMD_SET = EMU_CMD_RSTCAUSECLR;

	CMU->CLKEN0_SET = CMU_CLKEN0_GPIO | CMU_CLKEN0_HFRCO0 | CMU_CLKEN0_TIMER0 | CMU_CLKEN0_USART0;
	CMU->CLKEN1_SET = CMU_CLKEN1_MSC;

	/* one flash wait state first (WS0 holds up to 39 MHz: on the safe side) */
	MSC->LOCK = MSC_LOCK_LOCKKEY_UNLOCK;
	MSC->READCTRL = (MSC->READCTRL & ~_MSC_READCTRL_MODE_MASK) | MSC_READCTRL_MODE_WS1;

	uint32_t sysclk;

	if (hfxo_start()) {
		/* SYSCLK and peripheral groups A (TIMER0) and C (USART0) from the crystal */
		sysclk = CMU_SYSCLKCTRL_CLKSEL_HFXO;
		CMU->EM01GRPACLKCTRL = (CMU->EM01GRPACLKCTRL & ~_CMU_EM01GRPACLKCTRL_CLKSEL_MASK) |
				       CMU_EM01GRPACLKCTRL_CLKSEL_HFXO;
		CMU->EM01GRPCCLKCTRL = (CMU->EM01GRPCCLKCTRL & ~_CMU_EM01GRPCCLKCTRL_CLKSEL_MASK) |
				       CMU_EM01GRPCCLKCTRL_CLKSEL_HFXO;
		efr32_hz = EFR32_HFXO_HZ;
	} else {
		/* HFRCODPLL from 19 to 38 MHz (its calibration from DEVINFO, band 12 as emlib's
		 * CMU_HFRCODPLLBandSet); TIMER0 and USART0 run from it after reset */
		while (HFRCO0->STATUS & (HFRCO_STATUS_SYNCBUSY | HFRCO_STATUS_FREQBSY)) {
		}
		HFRCO0->CAL = DEVINFO->HFRCODPLLCAL[12].HFRCODPLLCAL & ~_HFRCO_CAL_CLKDIV_MASK;
		while (HFRCO0->STATUS & (HFRCO_STATUS_SYNCBUSY | HFRCO_STATUS_FREQBSY)) {
		}
		sysclk = CMU_SYSCLKCTRL_CLKSEL_HFRCODPLL;
		efr32_hz = 38000000u;
	}
	CMU->SYSCLKCTRL = (CMU->SYSCLKCTRL & ~(_CMU_SYSCLKCTRL_CLKSEL_MASK | _CMU_SYSCLKCTRL_PCLKPRESC_MASK |
					       _CMU_SYSCLKCTRL_HCLKPRESC_MASK)) |
			  sysclk;
	SystemCoreClock = efr32_hz;

	/* TIMER0: f / (f / 1 MHz) = 1 MHz, 32 bits; the overflow interrupt counts the wraps. CFG
	 * is written while the timer is off. */
	TIMER0->EN_CLR = TIMER_EN_EN;
	TIMER0->CFG = ((efr32_hz / 1000000u - 1u) << _TIMER_CFG_PRESC_SHIFT) | _TIMER_CFG_MODE_UP;
	TIMER0->EN_SET = TIMER_EN_EN;
	TIMER0->TOP = 0xFFFFFFFFu;
	TIMER0->CNT = 0;
	TIMER0->IF_CLR = _TIMER_IF_MASK;
	TIMER0->IEN = TIMER_IEN_OF;
	NVIC_SetPriority(TIMER0_IRQn, 1);
	NVIC_EnableIRQ(TIMER0_IRQn);
	TIMER0->CMD = TIMER_CMD_START;
}

uint64_t efr32_time_us(void)
{
	uint32_t high, low;

	/* a wrap between the two reads: read again (as the PIC's TMR2 with its overflow count) */
	do {
		high = overflows;
		low = TIMER0->CNT;
	} while (high != overflows || (low < 16u && (TIMER0->IF & TIMER_IF_OF)));

	return ((uint64_t)high << 32) | low;
}

void efr32_delay_us(uint32_t us)
{
	uint32_t start = TIMER0->CNT;

	while (TIMER0->CNT - start < us) {
	}
}

void efr32_reset(void)
{
	NVIC_SystemReset();
}

uint32_t efr32_reset_reason(void)
{
	return reset_reason;
}

/* ---- GPIO ----------------------------------------------------------------------------- */

static void gpio_mode(uint8_t pin, uint32_t mode)
{
	uint8_t port = pin >> 4, n = pin & 15u;
	volatile uint32_t *reg = (n < 8u) ? &GPIO->P[port].MODEL : &GPIO->P[port].MODEH;
	uint32_t shift = 4u * (n & 7u);

	*reg = (*reg & ~(0xFu << shift)) | (mode << shift);
}

void efr32_gpio_output(uint8_t pin, bool high)
{
	efr32_gpio_set(pin, high);
	gpio_mode(pin, _GPIO_P_MODEL_MODE0_PUSHPULL);
}

void efr32_gpio_input(uint8_t pin, bool pull_up)
{
	/* INPUTPULL pulls the way DOUT says */
	efr32_gpio_set(pin, pull_up);
	gpio_mode(pin, pull_up ? _GPIO_P_MODEL_MODE0_INPUTPULL : _GPIO_P_MODEL_MODE0_INPUT);
}

void efr32_gpio_set(uint8_t pin, bool high)
{
	if (high) {
		GPIO->P_SET[pin >> 4].DOUT = 1u << (pin & 15u);
	} else {
		GPIO->P_CLR[pin >> 4].DOUT = 1u << (pin & 15u);
	}
}

bool efr32_gpio_get(uint8_t pin)
{
	return (GPIO->P[pin >> 4].DIN >> (pin & 15u)) & 1u;
}

/* ---- USART0 as a tty ------------------------------------------------------------------ */

static efr32_tty_t *usart0_tty;

static bool baud_set(uint32_t baud)
{
	if (baud < 1200u || baud > efr32_hz / 16u) {
		return false;
	}

	/* emlib's USART_BaudrateAsyncSet, 16x oversampling: CLKDIV = 256 (f / (16 baud) - 1) */
	uint32_t div = (32u * efr32_hz + 8u * baud) / (16u * baud);

	div = (div - 32u) * 8u;
	USART0->CLKDIV = div & _USART_CLKDIV_DIV_MASK;
	return true;
}

/* fills the transmit buffer from the queue; interrupts off or from the interrupt */
static void tx_fill(efr32_tty_t *t)
{
	while (t->tx_head != t->tx_tail && (USART0->STATUS & USART_STATUS_TXBL)) {
		uint16_t head = t->tx_head;

		USART0->TXDATA = t->tx[head++];
		t->tx_head = (head == t->tx_size) ? 0u : head;
		t->out++;
	}

	if (t->tx_head == t->tx_tail) {
		USART0->IEN_CLR = USART_IEN_TXBL;
	} else {
		USART0->IEN_SET = USART_IEN_TXBL;
	}
}

void USART0_TX_IRQHandler(void)
{
	efr32_tty_t *t = usart0_tty;

	USART0->IF_CLR = USART_IF_TXBL;
	if (t != NULL) {
		tx_fill(t);
	} else {
		USART0->IEN_CLR = USART_IEN_TXBL;
	}
}

#if EFR32_TTY_LDMA_CH >= 0
/*
 * Received bytes go to the queue through the LDMA, one descriptor linked to itself: when the
 * queue's end is reached it starts over at the beginning. The descriptor is in RAM, as the
 * LDMA reads it again at each wrap, also while the flash is busy. The queue's tail is where
 * the LDMA writes next.
 */
static uint32_t rx_descriptor[4];

static void rx_dma_start(efr32_tty_t *t)
{
	const uint32_t ch = EFR32_TTY_LDMA_CH;
	const uint32_t mask = 1ul << ch;

	CMU->CLKEN0_SET = CMU_CLKEN0_LDMA | CMU_CLKEN0_LDMAXBAR;
	LDMA->EN_SET = LDMA_EN_EN;
	LDMA->CHDIS = mask;

	rx_descriptor[0] = LDMA_CH_CTRL_STRUCTTYPE_TRANSFER |
			   (((uint32_t)t->rx_size - 1u) << _LDMA_CH_CTRL_XFERCNT_SHIFT) |
			   LDMA_CH_CTRL_BLOCKSIZE_UNIT1 | LDMA_CH_CTRL_REQMODE_BLOCK |
			   LDMA_CH_CTRL_SRCINC_NONE | LDMA_CH_CTRL_SIZE_BYTE | LDMA_CH_CTRL_DSTINC_ONE;
	rx_descriptor[1] = (uint32_t)&USART0->RXDATA;
	rx_descriptor[2] = (uint32_t)t->rx;
	rx_descriptor[3] = (uint32_t)rx_descriptor | LDMA_CH_LINK_LINK; /* itself, absolute */

	LDMAXBAR->CH[ch].REQSEL =
		LDMAXBAR_CH_REQSEL_SOURCESEL_USART0 | LDMAXBAR_CH_REQSEL_SIGSEL_USART0RXDATAV;
	LDMA->CH[ch].LOOP = 0;
	LDMA->CH[ch].CFG = 0;
	LDMA->CH[ch].LINK = (uint32_t)rx_descriptor;
	LDMA->IEN_CLR = mask;
	LDMA->IF_CLR = mask;
	LDMA->REQDIS_CLR = mask;
	LDMA->CHDONE_CLR = mask;
	LDMA->LINKLOAD = mask; /* loads the descriptor and starts the channel */
}

/* where the LDMA writes the next byte */
static uint16_t rx_tail(const efr32_tty_t *t)
{
	uint32_t at = LDMA->CH[EFR32_TTY_LDMA_CH].DST - (uint32_t)t->rx;

	return (at < t->rx_size) ? (uint16_t)at : 0u;
}
#else
void USART0_RX_IRQHandler(void)
{
	efr32_tty_t *t = usart0_tty;

	/* the flags first: one cleared after the loop could be a byte's that came meanwhile,
	 * which would then wait for the next byte */
	uint32_t flags = USART0->IF;

	USART0->IF_CLR = flags & (USART_IF_RXDATAV | USART_IF_RXFULL | USART_IF_RXOF);
	while (USART0->STATUS & USART_STATUS_RXDATAV) {
		uint8_t b = (uint8_t)USART0->RXDATA;

		if (t == NULL) {
			continue;
		}

		uint16_t tail = t->rx_tail;
		uint16_t next = (uint16_t)(tail + 1u);

		next = (next == t->rx_size) ? 0u : next;
		if (next == t->rx_head) {
			t->lost++;
		} else {
			t->rx[tail] = b;
			t->rx_tail = next;
			t->in++;
		}
	}

	if ((flags & USART_IF_RXOF) && t != NULL) {
		t->lost++;
	}
}

static uint16_t rx_tail(const efr32_tty_t *t)
{
	return t->rx_tail;
}
#endif

static void tty_kick(efr32_tty_t *t)
{
	NVIC_DisableIRQ(USART0_TX_IRQn);
	tx_fill(t);
	NVIC_EnableIRQ(USART0_TX_IRQn);
}

void efr32_tty_put(efr32_tty_t *t, const void *bytes, unsigned int length)
{
	const uint8_t *data = bytes;

	for (unsigned int i = 0; i < length; i++) {
		uint16_t tail = t->tx_tail;
		uint16_t next = (uint16_t)(tail + 1u);

		next = (next == t->tx_size) ? 0u : next;

		/* more than the queue holds: this much has to wait */
		while (next == t->tx_head) {
			tty_kick(t);
		}

		t->tx[tail] = data[i];
		t->tx_tail = next;
	}

	tty_kick(t);
}

static int tty_write(void *args, void *buffer, unsigned int length)
{
	efr32_tty_put(args, buffer, length);
	return (int)length;
}

static int tty_read(void *args, void *buffer, unsigned int length)
{
	efr32_tty_t *t = args;
	uint8_t *data = buffer;
	unsigned int n = 0;

	uint16_t tail = rx_tail(t);

	while (n < length && t->rx_head != tail) {
		uint16_t head = t->rx_head;

		data[n++] = t->rx[head++];
		t->rx_head = (head == t->rx_size) ? 0u : head;
	}

#if EFR32_TTY_LDMA_CH >= 0
	t->in += n;

	if (USART0->IF & USART_IF_RXOF) {
		USART0->IF_CLR = USART_IF_RXOF;
		t->lost++;
	}
#endif

	return (int)n;
}

static int tty_execute(void *args, int argc, char *argv[])
{
	efr32_tty_t *t = args;

	if (argc == 2 && strcmp(argv[1], "-stat") == 0) {
		printf("%lu lost %lu in %lu out %lu\n", (unsigned long)t->baud, (unsigned long)t->lost,
		       (unsigned long)t->in, (unsigned long)t->out);
		return 0;
	}

	if (argc == 3 && strcmp(argv[1], "-b") == 0) {
		uint32_t baud = (uint32_t)atol(argv[2]);

		/* the old rate until what waits has gone */
		while (t->tx_head != t->tx_tail || !(USART0->STATUS & USART_STATUS_TXC)) {
			tty_kick(t);
		}
		if (!baud_set(baud)) {
			printf("invalid config\n");
			return -1;
		}
		t->baud = baud;
		return 0;
	}

	printf("usage: -b BAUD (1200 .. %lu), -stat\n", (unsigned long)(efr32_hz / 16u));

	return -1;
}

static const vfs_handler_t tty_node = {
	.read_handler = tty_read,
	.write_handler = tty_write,
	.execute_handler = tty_execute,
};

int efr32_tty_register(const char *name, efr32_tty_t *t)
{
	if (usart0_tty != NULL || (EFR32_TTY_LDMA_CH >= 0 && t->rx_size > 2048u)) {
		return -1;
	}

	t->tx_head = t->tx_tail = t->rx_head = t->rx_tail = 0;

	efr32_gpio_output(t->tx_pin, true);
	efr32_gpio_input(t->rx_pin, true);

	USART0->EN_SET = USART_EN_EN; /* the other registers need it */
	USART0->CMD = USART_CMD_RXDIS | USART_CMD_TXDIS | USART_CMD_CLEARRX | USART_CMD_CLEARTX;
	USART0->CTRL = USART_CTRL_OVS_X16;
	USART0->FRAME = USART_FRAME_DATABITS_EIGHT | USART_FRAME_PARITY_NONE | USART_FRAME_STOPBITS_ONE;
	if (!baud_set(t->baud)) {
		return -1;
	}

	GPIO->USARTROUTE[0].TXROUTE = ((uint32_t)(t->tx_pin >> 4) << _GPIO_USART_TXROUTE_PORT_SHIFT) |
				      ((uint32_t)(t->tx_pin & 15u) << _GPIO_USART_TXROUTE_PIN_SHIFT);
	GPIO->USARTROUTE[0].RXROUTE = ((uint32_t)(t->rx_pin >> 4) << _GPIO_USART_RXROUTE_PORT_SHIFT) |
				      ((uint32_t)(t->rx_pin & 15u) << _GPIO_USART_RXROUTE_PIN_SHIFT);
	GPIO->USARTROUTE[0].ROUTEEN = GPIO_USART_ROUTEEN_TXPEN | GPIO_USART_ROUTEEN_RXPEN;

	usart0_tty = t;
	USART0->IF_CLR = _USART_IF_MASK;
#if EFR32_TTY_LDMA_CH >= 0
	USART0->IEN = 0;
	rx_dma_start(t);
#else
	USART0->IEN = USART_IEN_RXDATAV;
	NVIC_SetPriority(USART0_RX_IRQn, 2);
	NVIC_EnableIRQ(USART0_RX_IRQn);
#endif
	USART0->CMD = USART_CMD_RXEN | USART_CMD_TXEN;
	NVIC_SetPriority(USART0_TX_IRQn, 2);
	NVIC_EnableIRQ(USART0_TX_IRQn);

	return vfs_node_new(name, false, t, &tty_node) < 0 ? -1 : 0;
}
