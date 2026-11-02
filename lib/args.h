/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file args.h
 * @brief Splitting command lines, and reading numbers from them.
 *
 * Used by system() to turn a command line into argc / argv.
 */

#ifndef ARGS_H
#define ARGS_H

#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Splits a command line into words, in place.
 *
 * Words are separated by spaces or tabs. A word in double quotes may hold
 * spaces: `set name "two words"` gives three words. The line is changed: a 0
 * is written after each word, and @p argv points into it.
 *
 * @param input    The command line; it is modified.
 * @param argv     Receives a pointer to each word.
 * @param max_args Size of @p argv; words after that are ignored.
 * @return The number of words.
 */
int args_parse(char *input, char *argv[], int max_args);

/**
 * @brief Parses a dotted path of byte values, such as "1.1.2.250".
 *
 * Numbers may be separated by dots or spaces.
 *
 * @param input      The text.
 * @param path       Receives the numbers.
 * @param max_length Size of @p path.
 * @return The number of values, or -1 when a value is outside 0..255.
 */
int args_parse_path(char *input, uint8_t *path, int max_length);

/**
 * @brief Reads a decimal number: [+-]digits[.digits][e[+-]digits].
 *
 * What strtod() does for a command line, at a tenth of its size: no hex, inf or
 * nan, and the last digit of a float may differ.
 *
 * @param text The text; blanks before the number are skipped.
 * @param end  Receives where the number ends, @p text when there is none; can be NULL.
 * @return The number, or 0 when there is none.
 */
float args_float(const char *text, char **end);

#endif /* ARGS_H */
