/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The tree over the fsi node table. Nothing is added to the table's entries;
 * a node's directory follows from its name and handlers:
 *
 *   name[0] & 0x80    in directory (name[0] & 0x7F), its name starts at name[1]
 *   plain name        in /dev when it can be read or written, else in /bin
 *   name[0] == 0xFF   an open file (directory 0x7F, which never exists)
 *
 * Directories and mount points are nodes whose handlers are vfs_dir_handlers
 * or vfs_mnt_handlers (no handler set); a mount point's args is its tfs_t.
 */

#include "vfs.h"

#include "../bin/tmp.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VFS_PREFIX    0x80u /* name[0] of a node in a vfs directory: 0x80 | directory */
#define VFS_FILE_MARK 0xFFu /* name[0] of an open file */
#define VFS_DEPTH_MAX 8u    /* deepest directory vfs_dir_path() prints */
#define VFS_VIRTUAL   2u    /* /bin and /dev, listed first in / */

extern fsi_handler_t tmp_node; /* bin/tmp.c: a RAM file */

static fsi_handler_t vfs_dir_handlers;
static fsi_handler_t vfs_mnt_handlers;

static uint8_t vfs_cwd_dir = VFS_ROOT;

/* -------------------------------------------------------------------------- */
/* nodes */

static fsi_node_t *vfs_at(int i)
{
	return fsi_nodes_get() + i;
}

static bool vfs_used(int i)
{
	fsi_node_t *n = vfs_at(i);

	return n->used == FSI_NODE_USED && n->handlers != NULL;
}

static bool vfs_prefixed(const fsi_node_t *n)
{
	return ((uint8_t)n->name[0] & VFS_PREFIX) != 0;
}

static uint8_t vfs_parent(const fsi_node_t *n)
{
	if (vfs_prefixed(n)) {
		return (uint8_t)n->name[0] & (uint8_t)~VFS_PREFIX;
	}

	/* a device (it can be read or written, maybe also commanded: ttyS1, a GPIO)
	 * is in /dev; a node that is only a command is in /bin */
	const fsi_handler_t *h = n->handlers;

	return (h->read_handler != NULL || h->write_handler != NULL) ? VFS_DEV : VFS_BIN;
}

static const char *vfs_name(const fsi_node_t *n)
{
	return n->name + (vfs_prefixed(n) ? 1 : 0);
}

static bool vfs_is_dir(const fsi_node_t *n)
{
	return n->handlers == &vfs_dir_handlers || n->handlers == &vfs_mnt_handlers;
}

/* a directory that exists */
static bool vfs_dir_ok(uint8_t dir)
{
	return dir < 3u || (dir < FSI_MAX_NODES_COUNT && vfs_used(dir) && vfs_is_dir(vfs_at(dir)));
}

static uint8_t vfs_dir_parent(uint8_t dir)
{
	return (dir < 3u) ? VFS_ROOT : vfs_parent(vfs_at(dir));
}

/* where fsi_node_create() will put the next node */
static int vfs_free_slot(void)
{
	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		if (vfs_at(i)->used == FSI_NODE_UNUSED) {
			return i;
		}
	}

	return -1;
}

static void vfs_drop(int i)
{
	vfs_at(i)->used = FSI_NODE_UNUSED;
	vfs_at(i)->args = NULL;
}

/* the node named @name in @dir, -1 if none */
static int vfs_child(uint8_t dir, const char *name)
{
	for (int i = 0; i < FSI_MAX_NODES_COUNT; i++) {
		if (vfs_used(i) && vfs_parent(vfs_at(i)) == dir &&
		    strcmp(vfs_name(vfs_at(i)), name) == 0) {
			return i;
		}
	}

	return -1;
}

static bool vfs_dir_empty(uint8_t dir)
{
	for (int i = 0; i < FSI_MAX_NODES_COUNT; i++) {
		if (vfs_used(i) && vfs_parent(vfs_at(i)) == dir) {
			return false;
		}
	}

	return vfs_mount_get(dir) == NULL;
}

/* the name a node in @dir gets in the table */
static int vfs_fullname(uint8_t dir, const char *name, char *full)
{
	size_t len = strlen(name);
	bool plain = (dir == VFS_BIN || dir == VFS_DEV);

	if (len == 0 || strchr(name, '/') != NULL || (uint8_t)name[0] >= VFS_PREFIX ||
	    len > FSI_NODE_NAME_MAX_LENGTH - (plain ? 1u : 2u)) {
		return -1;
	}

	if (plain) {
		strcpy(full, name);
	} else {
		full[0] = (char)(VFS_PREFIX | dir);
		strcpy(full + 1, name);
	}

	return 0;
}

static int vfs_node_add(uint8_t dir, const char *name, bool hidden, void *args,
			fsi_handler_t *handlers)
{
	char full[FSI_NODE_NAME_MAX_LENGTH];
	int i = vfs_free_slot();

	if (i < 0 || !vfs_dir_ok(dir) || vfs_mount_get(dir) != NULL || vfs_child(dir, name) >= 0 ||
	    vfs_fullname(dir, name, full) < 0) {
		return -1;
	}

	if (fsi_node_create(full, hidden, args, handlers) < 0) {
		return -1;
	}

	return i;
}

/* -------------------------------------------------------------------------- */
/* paths */

int vfs_resolve(const char *path, vfs_path_t *p)
{
	if (path == NULL || p == NULL) {
		return -1;
	}

	const char *s = path;
	uint8_t dir = (*s == '/') ? VFS_ROOT : vfs_cwd_dir;

	p->name[0] = '\0';

	for (;;) {
		while (*s == '/') {
			s++;
		}

		if (*s == '\0') {
			p->kind = VFS_DIR;
			p->dir = dir;
			p->node = (dir < 3u) ? -1 : dir;
			return 0;
		}

		size_t len = strcspn(s, "/");

		if (len >= TFS_NAME_SIZE) {
			return -1;
		}

		memcpy(p->name, s, len);
		p->name[len] = '\0';
		s += len;

		while (*s == '/') {
			s++;
		}

		bool last = (*s == '\0');

		if (strcmp(p->name, ".") == 0) {
			continue;
		}

		if (strcmp(p->name, "..") == 0) {
			dir = vfs_dir_parent(dir);
			continue;
		}

		if (dir == VFS_ROOT && strcmp(p->name, "bin") == 0) {
			dir = VFS_BIN;
			continue;
		}

		if (dir == VFS_ROOT && strcmp(p->name, "dev") == 0) {
			dir = VFS_DEV;
			continue;
		}

		int c = vfs_child(dir, p->name);

		if (c >= 0 && vfs_is_dir(vfs_at(c))) {
			dir = (uint8_t)c;
			continue;
		}

		if (!last) {
			return -1;
		}

		p->dir = dir;
		p->node = -1;
		p->kind = VFS_NONE;

		tfs_t *fs = vfs_mount_get(dir);
		tfs_info_t t;

		if (c < 0 && fs != NULL && tfs_stat(fs, p->name, &t) == 0) {
			p->kind = VFS_FILE;
			return 0;
		}

		/* a bare name: any plain node, as fsi does */
		bool bare = (strchr(path, '/') == NULL);

		for (int i = 0; c < 0 && bare && i < FSI_MAX_NODES_COUNT; i++) {
			if (vfs_used(i) && !vfs_prefixed(vfs_at(i)) &&
			    strcmp(vfs_at(i)->name, p->name) == 0) {
				c = i;
			}
		}

		if (c >= 0) {
			p->kind = VFS_NODE;
			p->node = (int16_t)c;
		}

		return 0;
	}
}

int vfs_dir_path(uint8_t dir, char *buffer, unsigned int size)
{
	uint8_t chain[VFS_DEPTH_MAX];
	unsigned int depth = 0;

	if (buffer == NULL || size < 2u) {
		return -1;
	}

	while (dir != VFS_ROOT && depth < VFS_DEPTH_MAX) {
		chain[depth++] = dir;
		dir = vfs_dir_parent(dir);
	}

	if (dir != VFS_ROOT) {
		return -1;
	}

	strcpy(buffer, "/");

	while (depth-- > 0) {
		uint8_t d = chain[depth];
		const char *name = (d == VFS_BIN)   ? "bin"
				   : (d == VFS_DEV) ? "dev"
						    : vfs_name(vfs_at(d));

		if (strlen(buffer) + strlen(name) + 2u > size) {
			return -1;
		}

		if (buffer[1] != '\0') {
			strcat(buffer, "/");
		}

		strcat(buffer, name);
	}

	return 0;
}

uint8_t vfs_cwd(void)
{
	return vfs_cwd_dir;
}

int vfs_chdir(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_DIR) {
		return -1;
	}

	vfs_cwd_dir = p.dir;

	return 0;
}

int vfs_dir_read(uint8_t dir, uint16_t *index, vfs_dirent_t *e)
{
	if (index == NULL || e == NULL || !vfs_dir_ok(dir)) {
		return -1;
	}

	memset(e, 0, sizeof(*e));
	e->node = -1;

	/* /bin and /dev, then the table, then the files of a mounted file system */
	while (*index < VFS_VIRTUAL + FSI_MAX_NODES_COUNT) {
		uint16_t i = (*index)++;

		if (i < VFS_VIRTUAL) {
			if (dir != VFS_ROOT) {
				continue;
			}

			strcpy(e->name, (i == 0) ? "bin" : "dev");
			e->kind = VFS_DIR;
			return 0;
		}

		int k = i - VFS_VIRTUAL;
		fsi_node_t *n = vfs_at(k);

		if (!vfs_used(k) || vfs_parent(n) != dir) {
			continue;
		}

		strcpy(e->name, vfs_name(n));
		e->kind = vfs_is_dir(n) ? VFS_DIR : VFS_NODE;
		e->mount = (n->handlers == &vfs_mnt_handlers);
		e->hidden = n->hidden;
		e->node = (int16_t)k;

		int size = tmp_file_size(n); /* a RAM file has a size too */

		if (size > 0) {
			e->size = (uint32_t)size;
		}
		return 0;
	}

	tfs_t *fs = vfs_mount_get(dir);
	uint8_t t = (uint8_t)(*index - VFS_VIRTUAL - FSI_MAX_NODES_COUNT);
	tfs_info_t info;

	if (fs == NULL || tfs_dir(fs, &t, &info) < 0) {
		return -1;
	}

	*index = (uint16_t)(VFS_VIRTUAL + FSI_MAX_NODES_COUNT + t);

	strcpy(e->name, info.name);
	e->kind = VFS_FILE;
	e->size = info.size;

	return 0;
}

/* -------------------------------------------------------------------------- */
/* nodes, commands, directories */

int vfs_node_create(const char *path, bool hidden, void *args, fsi_handler_t *handlers)
{
	vfs_path_t p;

	if (path == NULL || handlers == NULL) {
		return -1;
	}

	if (strchr(path, '/') == NULL) {
		int i = vfs_free_slot();

		return (fsi_node_create(path, hidden, args, handlers) < 0) ? -1 : i;
	}

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_NONE) {
		return -1;
	}

	return vfs_node_add(p.dir, p.name, hidden, args, handlers);
}

int vfs_register_app(const char *path, int (*execute_handler)(void *args, int argc, char *argv[]))
{
	if (path == NULL || execute_handler == NULL) {
		return -1;
	}

	if (strchr(path, '/') == NULL) {
		return fsi_register_app((char *)path, execute_handler);
	}

	fsi_handler_t *h = malloc(sizeof(fsi_handler_t));

	if (h == NULL) {
		return -1;
	}

	memset(h, 0, sizeof(*h));
	h->execute_handler = execute_handler;

	if (vfs_node_create(path, false, NULL, h) < 0) {
		free(h);
		return -1;
	}

	return 0;
}

int vfs_app_set(const char *name, int (*execute_handler)(void *args, int argc, char *argv[]))
{
	int i = vfs_child(VFS_BIN, name);

	if (i < 0) {
		return fsi_register_app((char *)name, execute_handler);
	}

	vfs_at(i)->handlers->execute_handler = execute_handler;

	return 0;
}

int vfs_mkdir(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_NONE || p.dir == VFS_BIN ||
	    p.dir == VFS_DEV) {
		return -1;
	}

	return vfs_node_add(p.dir, p.name, false, NULL, &vfs_dir_handlers) < 0 ? -1 : 0;
}

int vfs_remove(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0) {
		return -1;
	}

	if (p.kind == VFS_FILE) {
		return tfs_remove(vfs_mount_get(p.dir), p.name);
	}

	if (p.kind == VFS_NODE && vfs_at(p.node)->handlers == &tmp_node) {
		free(vfs_at(p.node)->args);
		vfs_drop(p.node);
		return 0;
	}

	if (p.kind == VFS_DIR && p.dir >= 3u && vfs_at(p.dir)->handlers == &vfs_dir_handlers &&
	    vfs_dir_empty(p.dir)) {
		if (vfs_cwd_dir == p.dir) {
			vfs_cwd_dir = vfs_dir_parent(p.dir);
		}

		vfs_drop(p.dir);
		return 0;
	}

	return -1;
}

int vfs_rename(const char *from, const char *to)
{
	vfs_path_t a, b;

	if (vfs_resolve(from, &a) < 0 || vfs_resolve(to, &b) < 0) {
		return -1;
	}

	/* into a directory: the same name there */
	const char *name = (b.kind == VFS_DIR) ? a.name : b.name;
	uint8_t dir = b.dir;

	if (a.kind == VFS_FILE) {
		if (dir != a.dir || b.kind == VFS_NODE) {
			return -1; /* another file system: cp */
		}

		return tfs_rename(vfs_mount_get(dir), a.name, name);
	}

	int i = (a.kind == VFS_NODE) ? a.node : (a.kind == VFS_DIR && a.dir >= 3u) ? a.dir : -1;
	char full[FSI_NODE_NAME_MAX_LENGTH];

	if (i < 0 || (b.kind != VFS_DIR && b.kind != VFS_NONE) || vfs_mount_get(dir) != NULL ||
	    vfs_child(dir, name) >= 0 || vfs_fullname(dir, name, full) < 0) {
		return -1;
	}

	/* not into itself */
	for (uint8_t d = dir; d != VFS_ROOT; d = vfs_dir_parent(d)) {
		if (d == i) {
			return -1;
		}
	}

	strcpy(vfs_at(i)->name, full);

	return 0;
}

int vfs_tmp_create(const vfs_path_t *p)
{
	char full[FSI_NODE_NAME_MAX_LENGTH];
	char *argv[2] = {"touch", full};
	int i = vfs_free_slot();

	if (p == NULL || p->kind != VFS_NONE || p->dir == VFS_BIN || !vfs_dir_ok(p->dir) ||
	    vfs_mount_get(p->dir) != NULL || vfs_fullname(p->dir, p->name, full) < 0) {
		return -1;
	}

	return (tmp_file_create(NULL, 2, argv) < 0) ? -1 : i;
}

/* -------------------------------------------------------------------------- */
/* block devices */

static int vfs_blk_read(void *args, void *buffer, unsigned int length)
{
	vfs_blk_t *b = args;

	if (b->pos >= b->size) {
		return 0;
	}

	if (length > b->size - b->pos) {
		length = (unsigned int)(b->size - b->pos);
	}

	if (b->read(b, b->pos, buffer, length) < 0) {
		return -1;
	}

	b->pos += length;

	return (int)length;
}

static int vfs_blk_write(void *args, void *buffer, unsigned int length)
{
	vfs_blk_t *b = args;

	if (b->pos >= b->size) {
		return -1;
	}

	if (length > b->size - b->pos) {
		length = (unsigned int)(b->size - b->pos);
	}

	if (b->write(b, b->pos, buffer, length) < 0) {
		return -1;
	}

	b->pos += length;

	return (int)length;
}

static int vfs_blk_lseek(int handle, off_t offset, int whence)
{
	vfs_blk_t *b = vfs_at(handle)->args;
	int32_t base = (whence == 0) ? 0 : (whence == 1) ? (int32_t)b->pos : (int32_t)b->size;

	if (base + (int32_t)offset < 0 || (uint32_t)(base + offset) > b->size) {
		return -1;
	}

	b->pos = (uint32_t)(base + offset);

	return (int)b->pos;
}

static fsi_handler_t vfs_blk_handlers = {
	.read_handler = vfs_blk_read,
	.write_handler = vfs_blk_write,
	.lseek_handler = vfs_blk_lseek,
};

int vfs_blk_register(const char *name, vfs_blk_t *blk)
{
	if (name == NULL || blk == NULL || strchr(name, '/') != NULL) {
		return -1;
	}

	return fsi_node_create(name, false, blk, &vfs_blk_handlers);
}

vfs_blk_t *vfs_blk_get(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_NODE ||
	    vfs_at(p.node)->handlers != &vfs_blk_handlers) {
		return NULL;
	}

	return vfs_at(p.node)->args;
}

/* -------------------------------------------------------------------------- */
/* open files */

static int vfs_file_read(void *args, void *buffer, unsigned int length)
{
	return tfs_read(args, buffer, length);
}

static int vfs_file_write(void *args, void *buffer, unsigned int length)
{
	return tfs_write(args, buffer, length);
}

static int vfs_file_lseek(int handle, off_t offset, int whence)
{
	return (int)tfs_seek(vfs_file_get(handle), (int32_t)offset, whence);
}

static int vfs_file_close(int handle)
{
	tfs_file_t *f = vfs_file_get(handle);

	if (f == NULL) {
		return -1;
	}

	int r = tfs_close(f);

	free(f);
	vfs_drop(handle);

	return r;
}

static fsi_handler_t vfs_file_handlers = {
	.read_handler = vfs_file_read,
	.write_handler = vfs_file_write,
	.lseek_handler = vfs_file_lseek,
	.close_handler = vfs_file_close,
};

tfs_file_t *vfs_file_get(int handle)
{
	if (handle < 0 || handle >= FSI_MAX_NODES_COUNT || !vfs_used(handle) ||
	    vfs_at(handle)->handlers != &vfs_file_handlers) {
		return NULL;
	}

	return vfs_at(handle)->args;
}

int vfs_file_open(const vfs_path_t *p, int flags)
{
	static const char mark[2] = {(char)VFS_FILE_MARK, '\0'};
	tfs_t *fs = (p == NULL) ? NULL : vfs_mount_get(p->dir);

	if (fs == NULL) {
		return -1;
	}

	/* flags 0: an fsi RAM file, reads from the start and writes append */
	uint8_t mode = TFS_WRITE;

	if ((flags & (O_WRONLY | O_RDWR)) == 0 || (flags & O_APPEND)) {
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
	int i = vfs_free_slot();

	if (f == NULL || i < 0 || tfs_open(fs, f, p->name, mode) < 0) {
		free(f);
		return -1;
	}

	if (fsi_node_create(mark, true, f, &vfs_file_handlers) < 0) {
		tfs_close(f);
		free(f);
		return -1;
	}

	return i;
}

/* -------------------------------------------------------------------------- */
/* mounts */

tfs_t *vfs_mount_get(uint8_t dir)
{
	if (dir < 3u || dir >= FSI_MAX_NODES_COUNT || !vfs_used(dir) ||
	    vfs_at(dir)->handlers != &vfs_mnt_handlers) {
		return NULL;
	}

	return vfs_at(dir)->args;
}

int vfs_mount(const char *dev, const char *path, bool format)
{
	vfs_blk_t *blk = vfs_blk_get(dev);
	vfs_path_t p;
	int d = -1;

	if (blk == NULL || vfs_resolve(path, &p) < 0) {
		return -1;
	}

	/* a device is mounted once */
	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		if (vfs_mount_get((uint8_t)i) != NULL && vfs_mount_get((uint8_t)i)->blk == blk) {
			return -1;
		}
	}

	if (p.kind == VFS_DIR && p.dir >= 3u && vfs_at(p.dir)->handlers == &vfs_dir_handlers &&
	    vfs_dir_empty(p.dir)) {
		d = p.dir;
	}

	bool created = false;

	if (p.kind == VFS_NONE && p.dir != VFS_BIN && p.dir != VFS_DEV) {
		d = vfs_node_add(p.dir, p.name, false, NULL, &vfs_dir_handlers);
		created = true;
	}

	if (d < 0) {
		return -1;
	}

	tfs_t *fs = malloc(sizeof(tfs_t));

	bool ok = (fs != NULL && tfs_mount(fs, blk) == 0);

	if (!ok && fs != NULL && format) {
		ok = tfs_format(blk, 0, 0) == 0 &&
		     tfs_mount(fs, blk) == 0;
	}

	if (!ok) {
		free(fs);

		if (created) {
			vfs_drop(d);
		}

		return -1;
	}

	vfs_at(d)->args = fs;
	vfs_at(d)->handlers = &vfs_mnt_handlers;

	return 0;
}

int vfs_umount(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_DIR) {
		return -1;
	}

	tfs_t *fs = vfs_mount_get(p.dir);

	if (fs == NULL) {
		return -1;
	}

	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		if (vfs_file_get(i) != NULL && vfs_file_get(i)->fs == fs) {
			return -1; /* a file is open */
		}
	}

	free(fs);
	vfs_at(p.dir)->args = NULL;
	vfs_at(p.dir)->handlers = &vfs_dir_handlers;

	return 0;
}

/* -------------------------------------------------------------------------- */

int vfs_initialize(void)
{
	vfs_cwd_dir = VFS_ROOT;

	if (vfs_mkdir("/app") < 0 || vfs_mkdir("/tmp") < 0 || vfs_mkdir("/mnt") < 0) {
		return -1;
	}

	return 0;
}
