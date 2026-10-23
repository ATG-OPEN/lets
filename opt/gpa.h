/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file gpa.h
 * @brief GPA: one I/O point (a GPIO pin, an ADC or a DAC channel) as a node.
 *
 * A gpa_t ties a channel number to the board's read and write functions
 * and to a linear scale, so the point is read and written in physical
 * units (volts, degrees, 0/1, ...) as a float:
 *
 *     value = (raw - read_offset) x read_k            (read)
 *     raw   = (value - write_offset) x write_k        (write)
 *
 * Registered as a node, the point can be used from C with read() and
 * write() of one float, and from the shell:
 *
 *     pot -i -s                     prints the value once
 *     pot -i -l <delay> <delta>     a task that prints the value when it
 *                                   changes by delta, or every delay s
 *     led1 -o <n> <delay> <value> [<delay> <value> ...]
 *                                   a task that writes the sequence n times
 *                                   (n < 0: forever), waiting delay s
 *                                   before each value
 *     pot -c -i <offset> <k>        sets the read scale
 *     led1 -c -o <offset> <k>       sets the write scale
 *     pot -help
 *
 * The tasks are named after the node, so `kill <node>` stops them.
 *
 * @code
 * static gpa_t pot = {
 *         .channel = 0,
 *         .read = analog_read,          // int analog_read(int channel)
 *         .read_k = 3.3f / 4096,        // counts to volts
 * };
 * gpa_register_node("pot", &pot);
 * @endcode
 */

#ifndef GPA_H
#define GPA_H

/** One I/O point. Leave read or write NULL for an output or input only. */
typedef struct {
	int channel;			      /**< Passed to read and write. */
	int (*write)(int channel, int value); /**< Sets the raw value, < 0 on error. */
	int (*read)(int channel);	      /**< Returns the raw value. */
	float write_k;			      /**< Raw units per physical unit (write). */
	float write_offset;		      /**< Physical value written as raw 0. */
	float read_k;			      /**< Physical units per raw unit (read). */
	float read_offset;		      /**< Raw value read as 0. */
} gpa_t;

/**
 * @brief Creates the node @p name for the point @p args (a gpa_t).
 * @return 0, or -1 on a NULL argument or when the node can't be created.
 */
int gpa_register_node(char *name, void *args);

/**
 * @brief Writes one float from @p buffer, scaled, to the point.
 * @return @p length, or -1 on an error or a point without write.
 */
int gpa_write(void *args, void *buffer, unsigned int length);

/**
 * @brief Reads the point, scaled, into one float at @p buffer.
 * @return @p length, or -1 on an error or a point without read.
 */
int gpa_read(void *args, void *buffer, unsigned int length);

/** @brief The shell command of the node (see the file comment). */
int gpa_execute(void *args, int argc, char *argv[]);

#endif /* GPA_H */
