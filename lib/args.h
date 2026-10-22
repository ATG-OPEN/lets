/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file args.h
 * @brief Splitting and joining command lines.
 *
 * Used by system() to turn a command line into argc / argv, and by programs
 * that pass command lines on.
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
 * @brief Joins words into one command line (the reverse of args_parse()).
 *
 * Words are separated by one space; a word with a space or tab in it is put
 * in double quotes. The result is cut to fit and always 0 terminated.
 *
 * @param output  Receives the line.
 * @param max_len Size of @p output in bytes.
 * @param argc    Number of words.
 * @param argv    The words.
 * @return The length of the line.
 */
int args_combine(char *output, int max_len, int argc, char *argv[]);

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

#endif /* ARGS_H */
