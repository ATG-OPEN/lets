/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file medianfilter.h
 * @brief Running median of the last N samples, declared by a macro.
 *
 * A median filter removes single spikes (a wrong ADC reading, a lost bit)
 * that an average would only smear out. Each update returns the median of
 * the last SIZE samples, or of all samples so far until SIZE have arrived.
 *
 * @code
 * MEDIANFILTER_DEFINE(pressure, uint16_t, 7)  // at file scope
 *
 * MEDIANFILTER_INIT(pressure);
 * uint16_t p = MEDIANFILTER_UPDATE(pressure, adc_read());
 * @endcode
 *
 * MEDIANFILTER_DEFINE creates static storage (one ring buffer and one sort
 * buffer of SIZE elements) and two functions, so each filter lives in one
 * C file. An update sorts SIZE elements (insertion sort): keep SIZE small,
 * an odd number such as 5, 7 or 9.
 */

#ifndef MEDIANFILTER_H
#define MEDIANFILTER_H

#include <stdint.h>

/**
 * @brief Defines the filter NAME for samples of TYPE over SIZE samples.
 *
 * Creates buffer_NAME[] (the last samples), temp_NAME[] (sorting), index_NAME
 * (next write position), filled_NAME (SIZE samples seen) and the functions
 * MEDIANFILTER_INIT_NAME() and MEDIANFILTER_UPDATE_NAME().
 */
#define MEDIANFILTER_DEFINE(NAME, TYPE, SIZE)                         \
	static TYPE buffer_##NAME[SIZE];                              \
	static TYPE temp_##NAME[SIZE];                                \
	static uint16_t index_##NAME = 0;                             \
	static uint8_t filled_##NAME = 0;                             \
                                                                      \
	static inline void MEDIANFILTER_INIT_##NAME(void)             \
	{                                                             \
		index_##NAME = 0;                                     \
		filled_##NAME = 0;                                    \
		for (uint16_t i = 0; i < SIZE; i++) {                 \
			buffer_##NAME[i] = (TYPE)0;                   \
		}                                                     \
	}                                                             \
                                                                      \
	static inline TYPE MEDIANFILTER_UPDATE_##NAME(TYPE SAMPLE)    \
	{                                                             \
		buffer_##NAME[index_##NAME] = SAMPLE;                 \
		index_##NAME++;                                       \
                                                                      \
		if (index_##NAME >= SIZE) {                           \
			index_##NAME = 0;                             \
			filled_##NAME = 1;                            \
		}                                                     \
                                                                      \
		/* the samples so far, sorted in a copy */            \
		uint16_t count = filled_##NAME ? SIZE : index_##NAME; \
                                                                      \
		for (uint16_t i = 0; i < count; i++) {                \
			temp_##NAME[i] = buffer_##NAME[i];            \
		}                                                     \
                                                                      \
		for (uint16_t i = 1; i < count; i++) {                \
			TYPE key = temp_##NAME[i];                    \
			uint16_t j = i;                               \
			while (j > 0 && temp_##NAME[j - 1] > key) {   \
				temp_##NAME[j] = temp_##NAME[j - 1];  \
				j--;                                  \
			}                                             \
			temp_##NAME[j] = key;                         \
		}                                                     \
                                                                      \
		return temp_##NAME[count / 2];                        \
	}

/** @brief Clears the filter NAME. */
#define MEDIANFILTER_INIT(NAME) MEDIANFILTER_INIT_##NAME()

/** @brief Adds sample X to the filter NAME and returns the median. */
#define MEDIANFILTER_UPDATE(NAME, X) MEDIANFILTER_UPDATE_##NAME(X)

#endif /* MEDIANFILTER_H */
