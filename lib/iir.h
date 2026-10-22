/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file iir.h
 * @brief One 2nd-order IIR filter section (biquad), designed from two cut-offs.
 *
 * The design follows the RBJ "Audio EQ Cookbook":
 *
 *   low only   high-pass (Q 0.707, removes the offset)
 *   high only  low-pass  (Q 0.707)
 *   both       band-pass (centre sqrt(low high), 0 dB peak); high must be above low
 *   neither    off
 *
 * A cut-off at or above 0.45 x the sample rate counts as not set. The filter is
 *
 *     y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]      (a0 = 1)
 *
 * The float design and the float / int32 runs are in iir.c. The Q14 fixed
 * point section is header-only (static inline), so a small MCU can filter
 * without floating point and without linking iir.c. Runs start in the steady
 * state for a given constant input, so a trace with an offset has no start-up
 * step.
 *
 * @code
 * iir_biquad_t f;
 * iir_design(&f, 0, 5e6f, 50e6f);         // 5 MHz low-pass at 50 MS/s
 * iir_run_i32(&f, trace, n, offset);      // in place, no start-up step
 *
 * iir_q14_t q;                            // the same filter in fixed point
 * iir_to_q14(&f, &q);
 * iir_q14_state_t s;
 * iir_q14_reset(&q, &s, x0);
 * int32_t y = iir_q14_step(&q, &s, x);
 * @endcode
 *
 * No allocation, no platform dependencies (C99).
 */

#ifndef IIR_H
#define IIR_H

#include <stdint.h>

/** Kind of filter (iir_biquad_t::kind). */
enum {
	IIR_OFF = 0,  /**< No filter: runs leave the data alone. */
	IIR_LOWPASS,  /**< Low-pass. */
	IIR_HIGHPASS, /**< High-pass. */
	IIR_BANDPASS  /**< Band-pass. */
};

/** A designed filter: float coefficients. */
typedef struct {
	uint8_t kind; /**< IIR_*. */
	float b0;     /**< Coefficient of x[n]. */
	float b1;     /**< Coefficient of x[n-1]. */
	float b2;     /**< Coefficient of x[n-2]. */
	float a1;     /**< Coefficient of y[n-1] (a0 = 1). */
	float a2;     /**< Coefficient of y[n-2]. */
} iir_biquad_t;

/**
 * @brief Designs @p f from two cut-offs (0: not set).
 * @return Its kind; IIR_OFF means there is nothing to filter.
 */
uint8_t iir_design(iir_biquad_t *f, float low_hz, float high_hz, float sample_hz);

/** @brief Output / input for a constant input: 1 when off, 0 for a band-pass or high-pass. */
float iir_dc_gain(const iir_biquad_t *f);

/** @brief Filters @p v in place, starting in the steady state for a constant input @p x0. */
void iir_run_f(const iir_biquad_t *f, float *v, uint16_t n, float x0);

/** @brief As iir_run_f(), for int32_t data (the output is rounded). */
void iir_run_i32(const iir_biquad_t *f, int32_t *v, uint16_t n, float x0);

/* -------------------------------------------------------------------------- */
/*
 * Q14 fixed point: the coefficients x 16384, in int16_t. With |x| <= 8190 every
 * product and their sum fit 32 bits. The output is rounded (a plain >> 14
 * rounds down, and the feedback would turn that -0.5 into a constant offset of
 * several counts) and clamped to int16_t.
 */

#define IIR_Q14 16384 /**< 1.0 in Q14. */

/** Forces inlining even without optimisation: the fixed-point helpers sit in inner loops
    and XC16 builds at -O0. */
#if defined(__GNUC__)
#define IIR_INLINE static inline __attribute__((always_inline))
#else
#define IIR_INLINE static inline
#endif

/** A filter in Q14 fixed point. */
typedef struct {
	int16_t b[3]; /**< b0, b1, b2. */
	int16_t a[2]; /**< a1, a2. */
} iir_q14_t;

/**
 * @brief Converts @p f to Q14.
 * @return 1, or 0 (all coefficients zero) when @p f is off.
 */
uint8_t iir_to_q14(const iir_biquad_t *f, iir_q14_t *q);

/** @brief Steady-state output for a constant input @p x. */
IIR_INLINE int32_t iir_q14_dc(const iir_q14_t *q, int32_t x)
{
	int32_t num = (int32_t)q->b[0] + q->b[1] + q->b[2];
	int32_t den = (int32_t)IIR_Q14 + q->a[0] + q->a[1];
	return den ? x * num / den : 0;
}

/**
 * @brief One output into int32_t @p y, from the input and the history.
 *
 * All arguments are int32_t values. A macro for the hottest loops, where even
 * an inlined call costs time at -O0.
 */
#define IIR_Q14_OUT(y, b0, b1, b2, a1, a2, x, x1, x2, y1, y2)                               \
	do {                                                                                \
		(y) = ((b0) * (x) + (b1) * (x1) + (b2) * (x2) - (a1) * (y1) - (a2) * (y2) + \
		       (1 << 13)) >>                                                        \
		      14;                                                                   \
		if ((y) > 32767)                                                            \
			(y) = 32767;                                                        \
		if ((y) < -32768)                                                           \
			(y) = -32768;                                                       \
	} while (0)

/** @brief Function form of IIR_Q14_OUT(). */
IIR_INLINE int32_t iir_q14_out(int32_t b0, int32_t b1, int32_t b2, int32_t a1, int32_t a2,
			       int32_t x, int32_t x1, int32_t x2, int32_t y1, int32_t y2)
{
	int32_t y;
	IIR_Q14_OUT(y, b0, b1, b2, a1, a2, x, x1, x2, y1, y2);
	return y;
}

/** History of a Q14 filter: the last two inputs and outputs. */
typedef struct {
	int32_t x1; /**< Input one step back. */
	int32_t x2; /**< Input two steps back. */
	int32_t y1; /**< Output one step back. */
	int32_t y2; /**< Output two steps back. */
} iir_q14_state_t;

/** @brief Sets the history to the steady state for a constant input @p x0. */
IIR_INLINE void iir_q14_reset(const iir_q14_t *q, iir_q14_state_t *s, int32_t x0)
{
	s->x1 = s->x2 = x0;
	s->y1 = s->y2 = iir_q14_dc(q, x0);
}

/** @brief Filters one sample. */
IIR_INLINE int32_t iir_q14_step(const iir_q14_t *q, iir_q14_state_t *s, int32_t x)
{
	int32_t y = iir_q14_out(q->b[0], q->b[1], q->b[2], q->a[0], q->a[1], x, s->x1, s->x2, s->y1,
				s->y2);
	s->x2 = s->x1;
	s->x1 = x;
	s->y2 = s->y1;
	s->y1 = y;
	return y;
}

#endif /* IIR_H */
