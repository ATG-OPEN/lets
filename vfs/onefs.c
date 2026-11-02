/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * "one": a block device that holds one file, rc. For memories too small for
 * tfs (a 256-byte I2C EEPROM: tfs's directory alone would take half of it),
 * mounted as /boot by vfs_boot("one", "ee0").
 *
 * Layout: magic "R1", the size (u16), a CRC-16 of the data (u16), the data.
 * The file is read whole into RAM when it is opened (the device's size at
 * most) and written back at close, its header last. A power loss before the
 * header is written leaves a CRC that does not match: rc is then missing and
 * the board runs its default rc.
 */

#include "vfs.h"

#include <stdlib.h>
#include <string.h>

#define ONEFS_HEADER 6u
#define ONEFS_NAME   "rc"

/* a mounted device */
typedef struct {
	vfs_blk_t *blk;
	uint16_t size; /* the file's size, when it is there */
	bool exists;
} onefs_t;

/* an open file: all of it in RAM */
typedef struct {
	onefs_t *fs;
	uint8_t *data;
	uint16_t size;
	uint16_t pos;
	uint8_t flags;
	bool dirty;
} onefs_file_t;

/* CRC-16/MODBUS, on from @crc (0xFFFF to start) */
static uint16_t onefs_crc(uint16_t crc, const uint8_t *p, uint16_t n)
{
	while (n--) {
		crc ^= *p++;

		for (int i = 0; i < 8; i++) {
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
		}
	}

	return crc;
}

static uint16_t onefs_capacity(const onefs_t *fs)
{
	uint32_t c = fs->blk->size - ONEFS_HEADER;

	return (uint16_t)((c > 0xFFFFu) ? 0xFFFFu : c);
}

/* reads the header and checks the data's CRC */
static void onefs_scan(onefs_t *fs)
{
	uint8_t h[ONEFS_HEADER];
	uint8_t b[16];

	fs->exists = false;

	if (fs->blk->read(fs->blk, 0, h, ONEFS_HEADER) < 0 || h[0] != 'R' || h[1] != '1') {
		return;
	}

	uint16_t size = (uint16_t)(h[2] | (h[3] << 8));
	uint16_t crc = 0xFFFFu;

	if (size > onefs_capacity(fs)) {
		return;
	}

	/* the CRC, a piece at a time */
	for (uint16_t at = 0; at < size;) {
		uint16_t n = (size - at < sizeof(b)) ? (uint16_t)(size - at) : (uint16_t)sizeof(b);

		if (fs->blk->read(fs->blk, ONEFS_HEADER + at, b, n) < 0) {
			return;
		}

		crc = onefs_crc(crc, b, n);
		at += n;
	}

	if (crc == (uint16_t)(h[4] | (h[5] << 8))) {
		fs->size = size;
		fs->exists = true;
	}
}

static void *onefs_mount(const char *source, bool format)
{
	vfs_blk_t *blk = vfs_blk_get(source);
	onefs_t *fs = (blk == NULL || blk->size <= ONEFS_HEADER) ? NULL : malloc(sizeof(onefs_t));

	if (fs != NULL) {
		fs->blk = blk;
		onefs_scan(fs); /* no file is an empty file system: nothing to format */
	}

	return fs;
}

static void onefs_umount(void *fs)
{
	free(fs);
}

static int onefs_info(void *fs, const char *name, vfs_info_t *info)
{
	onefs_t *o = fs;

	if (!o->exists || strcmp(name, ONEFS_NAME) != 0) {
		return -1;
	}

	info->size = o->size;
	info->crc = 0;
	info->has_crc = false;

	return 0;
}

static int onefs_dir(void *fs, uint16_t *index, char *name, vfs_info_t *info)
{
	if (*index > 0 || onefs_info(fs, ONEFS_NAME, info) < 0) {
		return -1;
	}

	strcpy(name, ONEFS_NAME);
	*index = 1;

	return 0;
}

static void *onefs_open(void *fs, const char *name, int flags)
{
	onefs_t *o = fs;

	if (strcmp(name, ONEFS_NAME) != 0 || (!o->exists && !(flags & O_CREAT)) ||
	    (o->exists && (flags & O_CREAT) && (flags & O_EXCL))) {
		return NULL;
	}

	onefs_file_t *f = malloc(sizeof(onefs_file_t));
	uint8_t *data = (f == NULL) ? NULL : malloc(onefs_capacity(o));

	if (data == NULL) {
		free(f);
		return NULL;
	}

	f->fs = o;
	f->data = data;
	f->size = 0;
	f->pos = 0;
	f->flags = (uint8_t)flags;
	f->dirty = !o->exists; /* O_CREAT: there is a file at close, empty or not */

	if (o->exists && !(flags & O_TRUNC)) {
		if (o->blk->read(o->blk, ONEFS_HEADER, data, o->size) < 0) {
			free(data);
			free(f);
			return NULL;
		}

		f->size = o->size;
	}

	if (flags & O_TRUNC) {
		f->dirty = true;
	}

	return f;
}

static int onefs_read(void *file, void *buffer, unsigned int length)
{
	onefs_file_t *f = file;
	unsigned int n = (unsigned int)(f->size - f->pos);

	if (n > length) {
		n = length;
	}

	memcpy(buffer, f->data + f->pos, n);
	f->pos += (uint16_t)n;

	return (int)n;
}

static int onefs_write(void *file, const void *buffer, unsigned int length)
{
	onefs_file_t *f = file;
	uint16_t capacity = onefs_capacity(f->fs);

	if (f->flags & O_APPEND) {
		f->pos = f->size;
	}

	if (f->pos >= capacity) {
		return -1;
	}

	unsigned int n = capacity - f->pos;

	if (n > length) {
		n = length;
	}

	memcpy(f->data + f->pos, buffer, n);
	f->pos += (uint16_t)n;

	if (f->pos > f->size) {
		f->size = f->pos;
	}

	f->dirty = true;

	return (int)n;
}

static int32_t onefs_seek(void *file, int32_t offset, int whence)
{
	onefs_file_t *f = file;
	int32_t base = (whence == 0) ? 0 : (whence == 1) ? f->pos : f->size;

	if (base + offset < 0 || base + offset > f->size) {
		return -1;
	}

	f->pos = (uint16_t)(base + offset);

	return f->pos;
}

/* the data, then the header */
static int onefs_close(void *file)
{
	onefs_file_t *f = file;
	onefs_t *o = f->fs;
	int r = 0;

	if (f->dirty) {
		uint16_t crc = onefs_crc(0xFFFFu, f->data, f->size);
		uint8_t h[ONEFS_HEADER] = {'R', '1', (uint8_t)f->size, (uint8_t)(f->size >> 8), (uint8_t)crc,
					   (uint8_t)(crc >> 8)};

		if ((f->size > 0 && o->blk->write(o->blk, ONEFS_HEADER, f->data, f->size) < 0) ||
		    o->blk->write(o->blk, 0, h, ONEFS_HEADER) < 0) {
			r = -1;
		}

		onefs_scan(o);
	}

	free(f->data);
	free(f);

	return r;
}

/* nothing was written to the device yet: rc stays as it was */
static int onefs_abort(void *file)
{
	onefs_file_t *f = file;

	free(f->data);
	free(f);

	return 0;
}

static int onefs_remove(void *fs, const char *name)
{
	onefs_t *o = fs;
	uint8_t none[2] = {0, 0};

	if (!o->exists || strcmp(name, ONEFS_NAME) != 0 || o->blk->write(o->blk, 0, none, 2) < 0) {
		return -1;
	}

	o->exists = false;

	return 0;
}

static int onefs_usage(void *fs, uint32_t *size, uint32_t *avail)
{
	onefs_t *o = fs;

	*size = onefs_capacity(o);
	*avail = *size - (o->exists ? o->size : 0u);

	return 0;
}

static const char *onefs_source(void *fs)
{
	return vfs_blk_name(((onefs_t *)fs)->blk);
}

vfs_fs_type_t vfs_one_type = {
	.name = "one",
	.mount = onefs_mount,
	.umount = onefs_umount,
	.info = onefs_info,
	.dir = onefs_dir,
	.open = onefs_open,
	.read = onefs_read,
	.write = onefs_write,
	.seek = onefs_seek,
	.close = onefs_close,
	.abort = onefs_abort,
	.remove = onefs_remove,
	.rename = NULL,
	.usage = onefs_usage,
	.source = onefs_source,
};
