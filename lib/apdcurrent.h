/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file apdcurrent.h
 * @brief APD bias set by the current through the photodiode.
 *
 * An avalanche photodiode (APD) read by a DC-coupled transimpedance
 * amplifier shifts the amplifier's output level by its own current. With
 * no light (laser off) that current is the dark current times the
 * avalanche gain, so it says directly how deep into the avalanche the
 * diode is biased, independent of temperature and of the individual
 * diode. This controller finds and holds the bias voltage at which that
 * current equals a target:
 *
 * - OFFSET: the bias goes to apd_min, where the APD carries no current,
 *   and the output level there is the amplifier's own offset.
 * - SEARCH: the bias jumps to search_from (or just below a hint, such as
 *   the result of the last calibration for this target) and ramps up while
 *   the current is far below the target.
 * - TRIM:   averages a few frames while the bias holds still and steps in
 *   proportion to log(current / target), until the current is within the
 *   tolerance a few times in a row, or the time is up.
 * - TRACK:  the same, in small steps, to follow temperature drift.
 *
 * Current = (level - offset) x current_per_unit, where level is the mean
 * of the amplifier's output in any unit (ADC counts, say) and the scale
 * comes from the front end. For an amplifier of gain G after a sense
 * resistor R and an ADC of full scale V over N counts:
 *
 *     current_per_unit = V / N / G / R      (amperes per count)
 *
 * High-voltage supplies are often slow to settle on the last volt. The
 * controller therefore runs an outer loop on the measured bias: the set
 * point it returns is the voltage it wants plus a correction that
 * integrates the remaining error, so the supply arrives in a fraction of
 * its own settling time.
 *
 * @code
 * apdcurrent_params_t p;
 * apdcurrent_t c = { 0 };
 * apdcurrent_defaults(&p);
 * p.current_per_unit = 3.3f / 4096 / 21 / 2000 * 1e6f;      // uA per count
 * apdcurrent_start(&c, &p, 1.0f, 0, now_us());               // 1 uA
 *
 * // for every frame taken with the light source off:
 * if (apdcurrent_update(&c, &p, frame_mean, hv_measured(), now_us()))
 *         hv_set(c.apd);
 * @endcode
 *
 * No allocation, no platform dependencies (C99).
 */

#ifndef APDCURRENT_H
#define APDCURRENT_H

#include <stdint.h>

/** Phase of the controller (apdcurrent_t::phase). */
enum {
	APDCURRENT_IDLE = 0, /**< Not running: the caller sets the bias. */
	APDCURRENT_OFFSET,   /**< Measuring the amplifier offset at apd_min. */
	APDCURRENT_SEARCH,   /**< Ramping up towards the target current. */
	APDCURRENT_TRIM,     /**< Converging on the target current. */
	APDCURRENT_TRACK     /**< On target: following drift. */
};

/** Tuning. Start from apdcurrent_defaults(). Voltages in V, times in s. */
typedef struct {
	float current_per_unit; /**< Current per unit of level (sign = amplifier polarity). */
	float apd_min;		/**< Bias for the offset measurement. */
	float apd_max;		/**< Highest bias ever set. */
	float offset_below;	/**< The offset is taken once the bias reads below this. */
	float offset_max_age;	/**< An offset this recent is reused without going down. */
	float search_from;	/**< First search voltage when nothing is known. */
	float search_lead;	/**< Later searches start this far below the last result. */
	float ramp_rate; /**< Search ramp, V/s, below target / 4 (twice that below target / 20, half above). */
	float tolerance;       /**< On target: |current / target - 1| within this. */
	float trim_gain;       /**< Volts per e-fold of current error. */
	float trim_step;       /**< Largest step while trimming. */
	float track_step;      /**< Largest step while tracking. */
	float settle;	       /**< A trim window counts only if the bias moved less than this. */
	float servo_gain;      /**< Outer bias loop, per second (0 = off). */
	float servo_max;       /**< Largest outer-loop correction. */
	float smooth;	       /**< Weight of a new reading in the smoothed current (0..1]. */
	float timeout;	       /**< Calibration (OFFSET to TRIM) ends after this. */
	uint8_t offset_frames; /**< Frames averaged for the offset. */
	uint8_t step_frames;   /**< Frames averaged for each trim decision. */
	uint8_t on_target;     /**< Decisions in tolerance in a row that end the calibration. */
} apdcurrent_params_t;

/** Controller state. Read the fields, change them only through the functions. */
typedef struct {
	uint8_t phase;	     /**< APDCURRENT_*. */
	uint8_t have_offset; /**< offset is known. */
	uint8_t reached;     /**< The last calibration reached the target (not the timeout). */
	uint8_t count; /**< Offset frames so far (OFFSET), decisions on target in a row (TRIM). */
	uint8_t have_current; /**< current holds a reading. */
	uint8_t window_n;     /**< Readings in the trim window. */
	float window_sum;     /**< Their current, summed. */
	float window_lo;      /**< Lowest measured bias in the window. */
	float window_hi;      /**< Highest measured bias in the window. */
	float target;	      /**< Target current. */
	float offset;	      /**< Level with no APD current. */
	float offset_sum;     /**< Offset frames summed so far. */
	float current;	      /**< Smoothed current, same unit as target. */
	float want;	      /**< Bias voltage the controller wants. */
	float servo;	      /**< Outer-loop correction added to want. */
	float apd;	      /**< The set point: apply it to the HV supply. */
	float result;	      /**< Bias at the end of the last calibration. */
	float hint;	      /**< Expected bias given to apdcurrent_start() (0 = none). */
	float last_v;	      /**< Measured bias at the previous update. */
	uint64_t offset_time; /**< When the offset was measured, us. */
	uint64_t start_time;  /**< When the calibration started, us. */
	uint64_t last_time;   /**< Previous update, us. */
} apdcurrent_t;

/**
 * @brief Defaults for a ~180 V APD behind a slow HV supply: offset below
 *        100 V, search from 170 V at 6 V/s, 25 % tolerance, 10 s timeout.
 *
 * current_per_unit is 1 (set it for the front end).
 */
void apdcurrent_defaults(apdcurrent_params_t *p);

/**
 * @brief Starts a calibration.
 * @param c      The controller.
 * @param p      Its tuning.
 * @param target Target current (same unit as the current).
 * @param hint   Expected bias, e.g. c->result of the last calibration for
 *               this target: the search starts search_lead below it
 *               (0 = start at search_from).
 * @param now    Time in microseconds.
 *
 * The offset is measured again unless a recent one is reused.
 */
void apdcurrent_start(apdcurrent_t *c, const apdcurrent_params_t *p, float target, float hint,
		      uint64_t now);

/** @brief Stops the controller (phase IDLE); what it learned is kept. */
void apdcurrent_stop(apdcurrent_t *c);

/** @brief Forgets the offset and the hint. */
void apdcurrent_forget(apdcurrent_t *c);

/** @brief Returns 1 from apdcurrent_start() until the calibration ends. */
uint8_t apdcurrent_calibrating(const apdcurrent_t *c);

/**
 * @brief Current for a level, using the known offset (0 without one).
 * @param c     The controller.
 * @param p     Its tuning.
 * @param level Mean output of the amplifier.
 * @return (level - offset) x current_per_unit.
 */
float apdcurrent_of(const apdcurrent_t *c, const apdcurrent_params_t *p, float level);

/**
 * @brief Feeds one frame taken with the light source off (or a level that
 *        stands for it while tracking).
 * @param c            The controller.
 * @param p            Its tuning.
 * @param level        Mean output of the amplifier during the frame.
 * @param apd_measured Bias voltage measured during the frame.
 * @param now          Time in microseconds.
 * @return 1 when the set point c->apd changed.
 */
uint8_t apdcurrent_update(apdcurrent_t *c, const apdcurrent_params_t *p, float level,
			  float apd_measured, uint64_t now);

/**
 * @brief Holds the bias found by the calibration without measuring the
 *        current (TRACK only).
 * @param c            The controller.
 * @param p            Its tuning.
 * @param apd_measured Bias voltage measured now.
 * @param now          Time in microseconds.
 * @return 1 when the set point c->apd changed.
 *
 * Runs only the outer loop on the measured bias, so a slow supply settles
 * on the calibrated voltage instead of the set point the search left
 * behind. Call it often (every 100 ms, say) between current measurements,
 * or instead of them when the bias must not follow the current.
 */
uint8_t apdcurrent_hold(apdcurrent_t *c, const apdcurrent_params_t *p, float apd_measured,
			uint64_t now);

#endif /* APDCURRENT_H */
