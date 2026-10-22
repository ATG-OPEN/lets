/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file ema_filter.h
 * @brief Exponential moving average in integer arithmetic (no floats).
 *
 * Smooths a stream of 16-bit samples (ADC readings, for example):
 *
 *     y = y + (x - y) / 2^shift
 *
 * The state is kept 2^shift times larger than the samples, so no precision
 * is lost to rounding. A larger shift smooths more and reacts more slowly:
 * after a step, the output reaches 63 % of it in about 2^shift samples.
 *
 * @code
 * ema_filter_t f;
 * ema_filter_init(&f, adc_read(), 4);        // alpha = 1/16
 *
 * uint16_t smooth = ema_filter_update(&f, adc_read());
 * @endcode
 *
 * With 16-bit samples, shift must be 16 or less so the state fits 32 bits.
 */

#ifndef EMA_FILTER_H
#define EMA_FILTER_H

#include <stdint.h>

/** Filter state. */
typedef struct {
	uint32_t accumulator; /**< The average x 2^shift. */
	uint8_t shift;	      /**< Smoothing: alpha = 1 / 2^shift. */
} ema_filter_t;

/**
 * @brief Starts the filter at a value.
 * @param filter        The filter.
 * @param initial_value The starting output (usually the first sample).
 * @param shift         Smoothing, alpha = 1 / 2^shift; typically 2 to 6.
 */
static inline void ema_filter_init(ema_filter_t *filter, uint16_t initial_value, uint8_t shift)
{
	filter->shift = shift;
	filter->accumulator = (uint32_t)initial_value << shift;
}

/**
 * @brief Adds a sample.
 * @return The new average.
 */
static inline uint16_t ema_filter_update(ema_filter_t *filter, uint16_t sample)
{
	/* acc = acc - acc / 2^shift + x, which is y += (x - y) / 2^shift scaled */
	filter->accumulator = filter->accumulator - (filter->accumulator >> filter->shift) + sample;

	return (uint16_t)(filter->accumulator >> filter->shift);
}

#endif /* EMA_FILTER_H */
