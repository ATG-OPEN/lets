/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "gpa.h"

#include "../kernel/task.h"
#include "../fsi/fsi.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

fsi_handler_t gpa_node = {
	.open_handler = NULL,
	.read_handler = gpa_read,
	.write_handler = gpa_write,
	.lseek_handler = NULL,
	.execute_handler = gpa_execute,
	.close_handler = NULL,
};

int gpa_register_node(char *name, void *args)
{
	if (name == NULL || args == NULL)
		return -1;

	if (fsi_node_create(name, false, args, &gpa_node) < 0)
		return -1;

	return 0;
}

int gpa_write(void *args, void *buffer, unsigned int length)
{
	if (args == NULL || buffer == NULL)
		return -1;

	if (length == 0)
		return 0;

	gpa_t *m = args;

	if (m->channel < 0 || m->write == NULL)
		return -1;

	float *data = buffer;
	int value = (int)((*data - m->write_offset) * m->write_k);

	if (m->write(m->channel, value) < 0)
		return -1;

	return length;
}

int gpa_read(void *args, void *buffer, unsigned int length)
{
	if (args == NULL || buffer == NULL)
		return -1;

	if (length == 0)
		return 0;

	gpa_t *m = args;

	if (m->channel < 0 || m->read == NULL)
		return -1;

	int value = m->read(m->channel);
	float *data = buffer;

	*data = ((float)value - m->read_offset) * m->read_k;

	return length;
}

/* ------------------------------------------------------------------------ */
/* `-i -l`: a task that prints the value when it changes                    */

typedef struct {
	char name[16];		   /* the node, also the task name */
	void *args;		   /* its gpa_t */
	float delta;		   /* print when the value moved this much */
	float delay;		   /* ... or after this many seconds (< 0.1 = never) */
	float value;		   /* the last value printed */
	uint64_t last_update_time; /* when it was printed, us */
} gpa_input_t;

static int gpa_input_loop(void *args, uint64_t system_time)
{
	if (args == NULL)
		return -1;

	gpa_input_t *g = args;
	float value;

	if (gpa_read(g->args, &value, 1) < 0)
		return -1;

	double elapsed_time = (double)(system_time - g->last_update_time) / 1000000;

	if (fabs(g->value - value) < g->delta) {
		if (elapsed_time < g->delay || g->delay < 0.1)
			return 10; /* unchanged: look again in 10 ms */
	}

	g->last_update_time = system_time;
	g->value = value;

	printf("%s %.2f\n", g->name, (double)value);

	return 10;
}

/* ------------------------------------------------------------------------ */
/* `-o`: a task that writes a sequence of values                            */

typedef struct {
	char name[16];	   /* the node, also the task name */
	void *args;	   /* its gpa_t */
	int count;	   /* repeats left (< 0 = forever) */
	int element_count; /* (delay, value) pairs */
	int element;	   /* the next pair */
	int state;	   /* 1 = the delay of the next pair has passed */
	float *delay;	   /* seconds before each value */
	float *value;	   /* the values */
} gpa_output_t;

static int gpa_output_loop(void *args, uint64_t system_time)
{
	if (args == NULL) {
		printf("-argument in null\n");
		return -1;
	}

	gpa_output_t *g = args;

	if (g->state > 0) {
		float value = g->value[g->element];

		if (gpa_write(g->args, &value, 1) < 0) {
			printf("-write error\n");
			return -1;
		}

		g->state = 0;
		g->element++;

		if (g->element >= g->element_count) {
			g->element = 0;

			if (g->count > 0)
				g->count--;
		}
	}

	if (g->count == 0)
		return -1; /* done: the task ends */

	int delay = 0;

	if (g->delay[g->element] > 0)
		delay = (int)(g->delay[g->element] * 1000);

	g->state = 1;

	return delay; /* run again when the next value is due, ms */
}

/* ------------------------------------------------------------------------ */

int gpa_execute(void *args, int argc, char *argv[])
{
	if (args == NULL)
		return -1;

	if (argc < 2) {
		printf("bad argument\n");
		return -1;
	}

	gpa_t *g = args;

	if (strcmp(argv[1], "-i") == 0) {
		if (argc < 3) {
			printf("bad argument\n");
			return -1;
		}

		if (strcmp(argv[2], "-s") == 0) {
			if (argc != 3) {
				printf("bad argument\n");
				return -1;
			}

			float value;

			if (gpa_read(args, &value, 1) < 0) {
				printf("read error\n");
				return -1;
			}

			printf("%s %.2f\n", argv[0], (double)value);

			return 0;
		} else if (strcmp(argv[2], "-l") == 0) {
			if (argc < 5) {
				printf("bad argument\n");
				return -1;
			}

			gpa_input_t *input = malloc(sizeof(gpa_input_t));

			if (input == NULL) {
				printf("out of memory\n");
				return -1;
			}

			input->args = args;
			input->delay = atof(argv[3]);
			input->delta = atof(argv[4]);
			input->value = 0;
			input->last_update_time = 0;

			strncpy(input->name, argv[0], sizeof(input->name) - 1);
			input->name[sizeof(input->name) - 1] = '\0';

			if (task_create(argv[0], gpa_input_loop, (void *)input) == NULL) {
				free(input);
				return -1;
			}

			return 0;
		}

		printf("bad argument\n");
		return -1;
	} else if (strcmp(argv[1], "-o") == 0) {
		if (argc < 5) {
			printf("bad argument\n");
			return -1;
		}

		int repeat_count = atoi(argv[2]);

		if (repeat_count == 0) {
			printf("bad argument\n");
			return -1;
		}

		int element_count = (argc - 3) / 2;

		/* one block: the struct, then the delays, then the values */
		gpa_output_t *output =
			malloc(sizeof(gpa_output_t) + 2 * sizeof(float) * element_count);

		if (output == NULL) {
			printf("out of memory\n");
			return -1;
		}

		uint8_t *buf = (uint8_t *)output;

		output->args = args;
		output->count = repeat_count;
		output->element_count = element_count;
		output->element = 0;
		output->state = 0;
		output->delay = (float *)(buf + sizeof(gpa_output_t));
		output->value =
			(float *)(buf + sizeof(gpa_output_t) + sizeof(float) * element_count);

		for (int i = 0; i < element_count; i++) {
			output->delay[i] = atof(argv[3 + i * 2]);
			output->value[i] = atof(argv[4 + i * 2]);
		}

		strncpy(output->name, argv[0], sizeof(output->name) - 1);
		output->name[sizeof(output->name) - 1] = '\0';

		if (task_create(argv[0], gpa_output_loop, (void *)output) == NULL) {
			free(output);
			return -1;
		}

		return 0;
	} else if (strcmp(argv[1], "-c") == 0) {
		if (argc < 5) {
			printf("bad argument\n");
			return -1;
		}

		if (strcmp(argv[2], "-i") == 0) {
			g->read_offset = atof(argv[3]);
			g->read_k = atof(argv[4]);
			return 0;
		} else if (strcmp(argv[2], "-o") == 0) {
			g->write_offset = atof(argv[3]);
			g->write_k = atof(argv[4]);
			return 0;
		}

		printf("bad argument\n");
		return -1;
	} else if (strcmp(argv[1], "-help") == 0) {
		printf("\t-o c [delay value]\n\t-i -s\n\t-i -l delay delta\n"
		       "\t-c -o offset k\n\t-c -i offset k\n");
		return 0;
	}

	return -1;
}
