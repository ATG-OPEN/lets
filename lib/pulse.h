/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file pulse.h
 * @brief Finds the returns (echo pulses) in a sampled trace.
 *
 * For laser rangefinders, lidar, sonar and similar: a pulse is sent, the
 * receiver is sampled, and each reflection shows up as a peak. pulse finds
 * the peaks that stand out from the noise, measures each one's time with a
 * constant-fraction (50 %) rising edge, and reports the strongest first.
 *
 * Input: int32_t samples that still hold their offset `dc` (subtracted here)
 * and a `scale` from sample units to the caller's units (for example mV per
 * shot after accumulating several shots).
 *
 * Noise: the trace is cut into 32-sample blocks; each block's RMS about its
 * own straight-line fit is taken (so the offset and slow slopes drop out), and
 * the lower quartile of those is the noise. A strong return only lifts the
 * blocks it is in, while noise that rises everywhere (an APD reaching
 * avalanche, for example) lifts all of them, which is what the noise should
 * report.
 *
 * Distance from a return: distance = index / sample_rate * c / 2.
 *
 * No allocation, no platform dependencies (C99).
 */

#ifndef PULSE_H
#define PULSE_H

#include <stdint.h>

#define PULSE_MAX_RETURNS 4	/**< Returns kept per trace. */
#define PULSE_BLOCK	  32	/**< Samples per noise block. */
#define PULSE_MAX_BLOCKS  64	/**< Blocks used for the noise (2048 samples). */
#define PULSE_SEPARATION  6	/**< A peak is the strongest within +- this many samples. */
#define PULSE_NOISE_MIN	  0.05f /**< Lowest noise reported (keeps the SNR finite). */

/** One return. */
typedef struct {
	float index;	 /**< Fractional sample of the 50 % rising edge. */
	float amplitude; /**< Peak above dc, scaled. */
} pulse_return_t;

/** Result of pulse_analyze(). */
typedef struct {
	float noise;			       /**< Robust noise, scaled. */
	float peak;			       /**< Strongest return (0 if none). */
	float snr;			       /**< peak / noise. */
	uint8_t count;			       /**< Returns found. */
	pulse_return_t ret[PULSE_MAX_RETURNS]; /**< The returns, strongest first. */
} pulse_analysis_t;

/**
 * @brief Measures the noise of v[from..count) (see the file description).
 * @return The noise, scaled; at least PULSE_NOISE_MIN.
 */
float pulse_noise(const int32_t *v, uint16_t count, int32_t dc, float scale, uint16_t from);

/**
 * @brief Finds the returns in a trace.
 *
 * A return is a local maximum above @p snr_detect x noise, the strongest
 * within +-PULSE_SEPARATION samples, at or after @p blank, whose 50 % edge is
 * not inside the blank zone either. The noise is measured after the blank
 * zone too, but always over at least the second half of the trace.
 *
 * @param v          Samples.
 * @param count      Number of samples.
 * @param dc         Offset in the samples.
 * @param scale      Sample units to the caller's units.
 * @param blank      Samples to ignore at the start (near-field reflections).
 * @param snr_detect Detection threshold in multiples of the noise.
 * @param a          Receives the result.
 */
void pulse_analyze(const int32_t *v, uint16_t count, int32_t dc, float scale, uint16_t blank,
		   float snr_detect, pulse_analysis_t *a);

/**
 * @brief Highest sample from @p from on (above dc, scaled), for int16_t data.
 * @param v     Samples.
 * @param count Number of samples.
 * @param dc    Offset in the samples.
 * @param from  First sample searched.
 * @param scale Sample units to the caller's units.
 * @param at    Receives its index; may be NULL.
 * @return The highest value.
 */
float pulse_max_i16(const int16_t *v, uint16_t count, int32_t dc, uint16_t from, float scale,
		    uint16_t *at);

/**
 * @brief Median of v[0..n). Sorts @p v in place (insertion sort: for small n).
 * @return The median, 0 when n is 0.
 */
float pulse_median(float *v, int n);

#endif /* PULSE_H */
