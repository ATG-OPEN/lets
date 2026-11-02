/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The tiny file system as a vfs file system type ("tfs"): `mount ee0 /mnt/ee`
 * or `mount -t tfs ee0 /mnt/ee` mounts the tfs on a block device. Also the
 * tfs commands mkfs and crc.
 */

#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TFSVFS_BUFFER 64u /* bytes read at a time by crc */

/* -------------------------------------------------------------------------- */

static void *tfsvfs_mount(const char *source, bool format)
{
	vfs_blk_t *blk = vfs_blk_get(source);
	tfs_t *fs = (blk == NULL) ? NULL : malloc(sizeof(tfs_t));

	if (fs == NULL) {
		return NULL;
	}

	if (tfs_mount(fs, blk) == 0 ||
	    (format && tfs_format(blk, 0, 0) == 0 && tfs_mount(fs, blk) == 0)) {
		return fs;
	}

	free(fs);

	return NULL;
}

static void tfsvfs_umount(void *fs)
{
	free(fs);
}

static int tfsvfs_info(void *fs, const char *name, vfs_info_t *info)
{
	tfs_info_t t;

	if (tfs_stat(fs, name, &t) < 0) {
		return -1;
	}

	info->size = t.size;
	info->crc = t.crc;
	info->has_crc = (t.attr & TFS_ATTR_CRC) != 0;

	return 0;
}

static int tfsvfs_dir(void *fs, uint16_t *index, char *name, vfs_info_t *info)
{
	uint8_t i = (uint8_t)*index;
	tfs_info_t t;

	if (*index > 0xFFu || tfs_dir(fs, &i, &t) < 0) {
		return -1;
	}

	*index = i;
	strcpy(name, t.name);
	info->size = t.size;
	info->crc = t.crc;
	info->has_crc = (t.attr & TFS_ATTR_CRC) != 0;

	return 0;
}

static void *tfsvfs_open(void *fs, const char *name, int flags)
{
	uint8_t mode = TFS_WRITE;

	if (flags & O_APPEND) {
		mode |= TFS_APPEND;
	}

	if (flags & O_CREAT) {
		mode |= TFS_CREATE;
	}

	if (flags & O_EXCL) {
		mode |= TFS_EXCL;
	}

	if (flags & O_TRUNC) {
		mode |= TFS_TRUNC;
	}

	tfs_file_t *f = malloc(sizeof(tfs_file_t));

	if (f == NULL || tfs_open(fs, f, name, mode) < 0) {
		free(f);
		return NULL;
	}

	return f;
}

static int tfsvfs_read(void *file, void *buffer, unsigned int length)
{
	return tfs_read(file, buffer, length);
}

static int tfsvfs_write(void *file, const void *buffer, unsigned int length)
{
	return tfs_write(file, buffer, length);
}

static int32_t tfsvfs_seek(void *file, int32_t offset, int whence)
{
	return tfs_seek(file, offset, whence);
}

static int tfsvfs_close(void *file)
{
	int r = tfs_close(file);

	free(file);

	return r;
}

static int tfsvfs_abort(void *file)
{
	int r = tfs_abort(file);

	free(file);

	return r;
}

static int tfsvfs_remove(void *fs, const char *name)
{
	return tfs_remove(fs, name);
}

static int tfsvfs_rename(void *fs, const char *from, const char *to)
{
	return tfs_rename(fs, from, to);
}

static int tfsvfs_usage(void *fs, uint32_t *size, uint32_t *avail)
{
	tfs_t *t = fs;

	*size = (uint32_t)t->blocks * t->bsize;
	*avail = tfs_free(t);

	return 0;
}

static const char *tfsvfs_source(void *fs)
{
	return vfs_blk_name(((tfs_t *)fs)->blk);
}

vfs_fs_type_t vfs_tfs_type = {
	.name = "tfs",
	.mount = tfsvfs_mount,
	.umount = tfsvfs_umount,
	.info = tfsvfs_info,
	.dir = tfsvfs_dir,
	.open = tfsvfs_open,
	.read = tfsvfs_read,
	.write = tfsvfs_write,
	.seek = tfsvfs_seek,
	.close = tfsvfs_close,
	.abort = tfsvfs_abort,
	.remove = tfsvfs_remove,
	.rename = tfsvfs_rename,
	.usage = tfsvfs_usage,
	.source = tfsvfs_source,
};

int vfs_mount(const char *dev, const char *path, bool format)
{
	return vfs_mount_type("tfs", dev, path, format);
}

/* -------------------------------------------------------------------------- */
/* commands */

/* crc path: size and CRC-32 of a file, checked against the stored one */
int vfs_tfs_crc(void *args, int argc, char *argv[])
{
	vfs_path_t p;
	vfs_info_t t;

	if (argc != 2 || vfs_resolve(argv[1], &p) < 0 || vfs_file_info(&p, &t) < 0) {
		printf("bad argument\n");
		return -1;
	}

	int fd = open(argv[1], 0, 0);
	uint8_t *buffer = malloc(TFSVFS_BUFFER);
	uint32_t crc = 0;
	int n = -1;

	while (fd >= 0 && buffer != NULL && (n = read(fd, buffer, TFSVFS_BUFFER)) > 0) {
		crc = tfs_crc32(crc, buffer, (unsigned int)n);
	}

	free(buffer);

	if (fd >= 0) {
		close(fd);
	}

	if (n < 0) {
		printf("open failed\n");
		return -1;
	}

	const char *check = !t.has_crc ? "" : (crc == t.crc) ? " ok" : " BAD";

	printf("%lu %08lX%s\n", (unsigned long)t.size, (unsigned long)crc, check);

	return (*check == ' ' && crc != t.crc) ? -1 : 0;
}

/* mkfs dev [block size] [entries] */
int vfs_tfs_mkfs(void *args, int argc, char *argv[])
{
	if (argc < 2 || argc > 4) {
		printf("usage: mkfs dev [block] [entries]\n");
		return -1;
	}

	vfs_blk_t *blk = vfs_blk_get(argv[1]);
	int bsize = (argc > 2) ? atoi(argv[2]) : 0; /* 0: by the device size */
	int entries = (argc > 3) ? atoi(argv[3]) : 0;

	if (blk == NULL || bsize < 0 || entries < 0 || entries > 255) {
		printf("bad argument\n");
		return -1;
	}

	for (int i = VFS_DIR_FIRST; i < VFS_DIR_FIRST + (int)VFS_DIRS_MAX; i++) {
		const vfs_fs_type_t *type;
		tfs_t *fs = vfs_mount_get((uint8_t)i, &type);

		if (fs != NULL && type == &vfs_tfs_type && fs->blk == blk) {
			printf("mounted\n");
			return -1;
		}
	}

	if (tfs_format(blk, (uint16_t)bsize, (uint8_t)entries) < 0) {
		printf("bad argument\n");
		return -1;
	}

	return 0;
}
