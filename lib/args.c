/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "args.h"

#include <stdbool.h>
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

float args_float(const char *text, char **end)
{
	const char *p = text;
	bool minus = false;
	bool digits = false;
	float value = 0;
	float place = 1;

	while (*p == ' ' || *p == '\t') {
		p++;
	}

	if (*p == '-' || *p == '+') {
		minus = (*p++ == '-');
	}

	for (; *p >= '0' && *p <= '9'; p++, digits = true) {
		value = value * 10 + (float)(*p - '0');
	}

	if (*p == '.') {
		for (p++; *p >= '0' && *p <= '9'; p++, digits = true) {
			value = value * 10 + (float)(*p - '0');
			place *= 10;
		}
	}

	if (!digits) {
		if (end != NULL) {
			*end = (char *)text;
		}

		return 0;
	}

	if (*p == 'e' || *p == 'E') {
		const char *e = p + 1;
		bool small = (*e == '-');
		int exponent = 0;

		e += (*e == '-' || *e == '+') ? 1 : 0;

		if (*e >= '0' && *e <= '9') {
			for (; *e >= '0' && *e <= '9'; e++) {
				exponent = (exponent < 100) ? exponent * 10 + (*e - '0') : exponent;
			}

			for (; exponent > 0; exponent--) {
				if (small) {
					place *= 10;
				} else {
					value *= 10;
				}
			}

			p = e;
		}
	}

	if (end != NULL) {
		*end = (char *)p;
	}

	value /= place;

	return minus ? -value : value;
}
