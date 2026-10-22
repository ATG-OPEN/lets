/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file apdbias.h
 * @brief Automatic bias voltage for an avalanche photodiode (APD).
 *
 * An APD's gain rises steeply with its bias voltage. The best sensitivity is
 * just below the avalanche edge, where the noise starts to rise; above it the
 * noise swamps the signal. The edge moves with temperature and background
 * light, so it is found and followed at run time:
 *
 * - FLOOR:  measures the noise floor at apd_min (no gain).
 * - SEARCH: steps up from search_from (search_step, then fine_step) until the
 *   noise reaches floor x edge_ratio. A coarse step past the edge goes back
 *   one step and repeats finely.
 * - TRACK:  holds `backoff` volts below the edge. Noise above floor x
 *   too_noisy_ratio steps down (and lowers the edge); quiet again creeps
 *   back up. With no backoff the edge itself creeps up, so a first guess
 *   that was too low is climbed out of.
 *
 * Feed it one noise value per measured frame, with the measured bias
 * voltage. Noise is judged only once the voltage is within `settle` of the
 * set point and `skip_*` frames after it moved (an HV supply that is still
 * ramping reads noisy), and as the median of frames_per_step frames.
 *
 * @code
 * apdbias_params_t p;
 * apdbias_t b;
 * apdbias_defaults(&p);
 * apdbias_start(&b, &p);
 *
 * // for every frame:
 * if (apdbias_update(&b, &p, frame_noise, hv_measured()))
 *         hv_set(b.apd);                   // new set point
 * @endcode
 *
 * No allocation, no platform dependencies (C99).
 */

#ifndef APDBIAS_H
#define APDBIAS_H

#include <stdint.h>

/** Phase of the controller (apdbias_t::phase). */
enum {
	APDBIAS_MANUAL = 0, /**< Not running: apd is set by the caller. */
	APDBIAS_FLOOR,	    /**< Measuring the noise floor. */
	APDBIAS_SEARCH,	    /**< Looking for the avalanche edge. */
	APDBIAS_TRACK	    /**< Holding below the edge. */
};

#define APDBIAS_MAX_FRAMES 8 /**< Most frames per median. */

/** Tuning. Start from apdbias_defaults(). Voltages in V. */
typedef struct {
	float apd_min;		 /**< Lowest bias (no gain: the noise floor). */
	float apd_max;		 /**< Highest bias. */
	float search_from;	 /**< First search voltage (a known edge starts higher). */
	float search_step;	 /**< Coarse search step. */
	float fine_step;	 /**< Fine search step. */
	float fine_from;	 /**< A search starting at or above this uses fine steps. */
	float search_lead;	 /**< A new search starts this far below the last edge. */
	float track_step;	 /**< Step while tracking. */
	float backoff;		 /**< Hold this far below the edge. */
	float edge_ratio;	 /**< The edge: noise = floor x this. */
	float too_noisy_ratio;	 /**< Tracking: noise above floor x this steps down. */
	float settle;		 /**< Judge noise only within this of the set point. */
	uint8_t frames_per_step; /**< Median of this many frames (<= APDBIAS_MAX_FRAMES). */
	uint8_t skip_jump;	 /**< Frames skipped after the jump from floor to search. */
	uint8_t skip_step;	 /**< Frames skipped after each search step. */
} apdbias_params_t;

/** Controller state. */
typedef struct {
	uint8_t phase;			 /**< APDBIAS_*. */
	uint8_t fine;			 /**< Searching with fine steps. */
	uint8_t have_floor;		 /**< noise_floor is known. */
	uint8_t have_edge;		 /**< edge is known. */
	uint8_t n_noise;		 /**< Frames collected for the median. */
	uint8_t skip;			 /**< Frames still to skip. */
	float apd;			 /**< The set point: apply it to the HV supply. */
	float noise_floor;		 /**< Learned noise at apd_min. */
	float edge;			 /**< Learned avalanche edge voltage. */
	float noise[APDBIAS_MAX_FRAMES]; /**< Frames collected for the median. */
} apdbias_t;

/**
 * @brief Conservative defaults: floor at 50 V, search from 100 V in 10 / 2 V
 *        steps, edge at 2 x floor, held 4 V below.
 */
void apdbias_defaults(apdbias_params_t *p);

/**
 * @brief Sets the edge ratio and the backoff (keeps too_noisy_ratio above the
 *        edge ratio).
 */
void apdbias_set_edge(apdbias_params_t *p, float ratio, float backoff);

/**
 * @brief Starts (or restarts) from the floor phase.
 *
 * A learned edge is kept and shortens the search.
 */
void apdbias_start(apdbias_t *b, const apdbias_params_t *p);

/** @brief Forgets the learned floor and edge. */
void apdbias_forget(apdbias_t *b);

/** @brief Returns 1 while measuring the floor or searching the edge. */
uint8_t apdbias_calibrating(const apdbias_t *b);

/**
 * @brief Feeds one frame.
 * @param b            The controller.
 * @param p            Its tuning.
 * @param noise        The frame's noise (any unit, the same every time).
 * @param apd_measured The bias voltage measured during the frame.
 * @return 1 when the set point b->apd changed.
 */
uint8_t apdbias_update(apdbias_t *b, const apdbias_params_t *p, float noise, float apd_measured);

#endif /* APDBIAS_H */
