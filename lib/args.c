/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "args.h"

#include <stdint.h>
#include <string.h>

int args_parse(char *input, char *argv[], int max_args)
{
	int argc = 0;
	char *p = input;

	while (*p != '\0' && argc < max_args) {
		/* skip the blanks before the word */
		while (*p == ' ' || *p == '\t') {
			p++;
		}

		if (*p == '\0') {
			break;
		}

		if (*p == '"') {
			/* quoted word: up to the closing quote, blanks included */
			p++;
			argv[argc++] = p;

			while (*p != '\0' && *p != '"') {
				p++;
			}

			if (*p == '"') {
				*p = '\0';
				p++;
			}
		} else {
			/* plain word: up to the next blank */
			argv[argc++] = p;

			while (*p != '\0' && *p != ' ' && *p != '\t') {
				p++;
			}

			if (*p != '\0') {
				*p = '\0';
				p++;
			}
		}
	}

	return argc;
}

int args_combine(char *output, int max_len, int argc, char *argv[])
{
	int pos = 0;

	for (int i = 0; i < argc; i++) {
		char *arg = argv[i];
		int needs_quote = 0;

		/* a word with a blank in it is quoted */
		for (char *p = arg; *p != '\0'; p++) {
			if (*p == ' ' || *p == '\t') {
				needs_quote = 1;
				break;
			}
		}

		/* one space between words; each step stops when the output is full */
		if (i > 0) {
			if (pos + 1 >= max_len) {
				break;
			}
			output[pos++] = ' ';
		}

		if (needs_quote) {
			if (pos + 1 >= max_len) {
				break;
			}
			output[pos++] = '"';
		}

		for (char *p = arg; *p != '\0'; p++) {
			if (pos + 1 >= max_len) {
				break;
			}
			output[pos++] = *p;
		}

		if (needs_quote) {
			if (pos + 1 >= max_len) {
				break;
			}
			output[pos++] = '"';
		}
	}

	if (pos < max_len) {
		output[pos] = '\0';
	} else {
		output[max_len - 1] = '\0';
	}

	return pos;
}

int args_parse_path(char *input, uint8_t *path, int max_length)
{
	int count = 0;
	char *p = input;

	while (*p != '\0' && count < max_length) {
		while (*p == ' ' || *p == '.') {
			p++;
		}

		if (*p == '\0') {
			break;
		}

		int value = strtol(p, &p, 10);

		if (value < 0 || value > 255) {
			return -1;
		}

		path[count++] = (uint8_t)value;
	}

	return count;
}
