/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The tree over the node table (node.c). The table holds only nodes (devices,
 * commands, RAM files), with nothing added to its entries; a node's directory
 * follows from its name and handlers:
 *
 *   name[0] & 0x80    in directory (name[0] & 0x7F), its name starts at name[1]
 *   plain name        in /dev when it can be read or written, else in /bin
 *
 * Directories and mount points are not nodes: they are in vfs_dirs, with ids
 * VFS_DIR_FIRST + k. Handles are not nodes either (vfs_fds), and the built-in
 * commands are in flash (vfs_commands). So vfs takes no place in the node table.
 */

#include "vfs.h"

#include "../bin/tmp.h"
#include "../kernel/task.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VFS_PREFIX    0x80u /* name[0] of a node in a vfs directory: 0x80 | directory */
#define VFS_DEPTH_MAX 8u    /* deepest directory vfs_dir_path() prints */
#define VFS_VIRTUAL   2u    /* /bin and /dev, listed first in / */

extern const vfs_handler_t tmp_node; /* bin/tmp.c: a RAM file */

void vfs_nodes_clear(void); /* node.c */

static bool vfs_node_busy(int node);

static const vfs_handler_t vfs_blk_handlers; /* defined with the block devices */

/* a directory, or a mount point when fs is set */
typedef struct {
	char name[VFS_DIR_NAME];     /* empty: a free entry */
	uint8_t parent;		     /* directory id */
	const vfs_fs_type_t *type;   /* the type of the file system mounted here */
	void *fs;		     /* the file system mounted here, or NULL */
} vfs_dir_t;

static vfs_fs_type_t *vfs_types; /* registered file system types */

static vfs_dir_t vfs_dirs[VFS_DIRS_MAX];

/* -------------------------------------------------------------------------- */
/* nodes */

static vfs_node_t *vfs_at(int i)
{
	return vfs_nodes_get() + i;
}

static bool vfs_used(int i)
{
	return vfs_at(i)->handlers != NULL;
}

static bool vfs_prefixed(const vfs_node_t *n)
{
	return ((uint8_t)n->name[0] & VFS_PREFIX) != 0;
}

static uint8_t vfs_parent(const vfs_node_t *n)
{
	if (vfs_prefixed(n)) {
		return (uint8_t)n->name[0] & (uint8_t)~VFS_PREFIX;
	}

	/* a device (it can be read or written, maybe also commanded: ttyS1, a GPIO)
	 * is in /dev; a node that is only a command is in /bin */
	const vfs_handler_t *h = n->handlers;

	return (h->read_handler != NULL || h->write_handler != NULL) ? VFS_DEV : VFS_BIN;
}

static const char *vfs_name(const vfs_node_t *n)
{
	return n->name + (vfs_prefixed(n) ? 1 : 0);
}

/* the entry of directory id @dir, NULL for /, /bin, /dev or a free id */
static vfs_dir_t *vfs_dir(uint8_t dir)
{
	unsigned int k = (unsigned int)dir - VFS_DIR_FIRST;

	if (dir < VFS_DIR_FIRST || k >= VFS_DIRS_MAX || vfs_dirs[k].name[0] == '\0') {
		return NULL;
	}

	return &vfs_dirs[k];
}

/* a directory that exists */
static bool vfs_dir_ok(uint8_t dir)
{
	return dir < VFS_DIR_FIRST || vfs_dir(dir) != NULL;
}

static uint8_t vfs_dir_parent(uint8_t dir)
{
	vfs_dir_t *d = vfs_dir(dir);

	return (d == NULL) ? VFS_ROOT : d->parent;
}

static void vfs_drop(int i)
{
	vfs_at(i)->handlers = NULL;
	vfs_at(i)->args = NULL;
}

/* the node named @name in @dir, -1 if none */
static int vfs_child(uint8_t dir, const char *name)
{
	for (int i = 0; i < VFS_NODES_MAX; i++) {
		if (vfs_used(i) && vfs_parent(vfs_at(i)) == dir &&
		    strcmp(vfs_name(vfs_at(i)), name) == 0) {
			return i;
		}
	}

	return -1;
}

/* the directory named @name in @dir (/bin and /dev too), -1 if none */
static int vfs_subdir(uint8_t dir, const char *name)
{
	if (dir == VFS_ROOT && strcmp(name, "bin") == 0) {
		return VFS_BIN;
	}

	if (dir == VFS_ROOT && strcmp(name, "dev") == 0) {
		return VFS_DEV;
	}

	for (unsigned int k = 0; k < VFS_DIRS_MAX; k++) {
		if (vfs_dirs[k].name[0] != '\0' && vfs_dirs[k].parent == dir &&
		    strcmp(vfs_dirs[k].name, name) == 0) {
			return (int)(VFS_DIR_FIRST + k);
		}
	}

	return -1;
}

static bool vfs_dir_empty(uint8_t dir)
{
	for (int i = 0; i < VFS_NODES_MAX; i++) {
		if (vfs_used(i) && vfs_parent(vfs_at(i)) == dir) {
			return false;
		}
	}

	for (unsigned int k = 0; k < VFS_DIRS_MAX; k++) {
		if (vfs_dirs[k].name[0] != '\0' && vfs_dirs[k].parent == dir) {
			return false;
		}
	}

	return vfs_mount_get(dir, NULL) == NULL;
}

/* a name for @dir: not there yet, no '/', not too long for a node there */
static bool vfs_name_free(uint8_t dir, const char *name, size_t max)
{
	size_t len = strlen(name);

	return len > 0 && len <= max && strchr(name, '/') == NULL && (uint8_t)name[0] < VFS_PREFIX &&
	       strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && vfs_dir_ok(dir) &&
	       vfs_mount_get(dir, NULL) == NULL && vfs_child(dir, name) < 0 && vfs_subdir(dir, name) < 0;
}

/* the name a node in @dir gets in the table */
static int vfs_fullname(uint8_t dir, const char *name, char *full)
{
	bool plain = (dir == VFS_BIN || dir == VFS_DEV);

	if (!vfs_name_free(dir, name, VFS_NODE_NAME_MAX - (plain ? 1u : 2u))) {
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
			const vfs_handler_t *handlers)
{
	char full[VFS_NODE_NAME_MAX];

	if (vfs_fullname(dir, name, full) < 0) {
		return -1;
	}

	return vfs_node_new(full, hidden, args, handlers);
}

/* a new directory: its id, or -1 */
static int vfs_dir_add(uint8_t dir, const char *name)
{
	if (dir == VFS_BIN || dir == VFS_DEV || !vfs_name_free(dir, name, VFS_DIR_NAME - 1u)) {
		return -1;
	}

	for (unsigned int k = 0; k < VFS_DIRS_MAX; k++) {
		if (vfs_dirs[k].name[0] == '\0') {
			strcpy(vfs_dirs[k].name, name);
			vfs_dirs[k].parent = dir;
			vfs_dirs[k].type = NULL;
			vfs_dirs[k].fs = NULL;
			return (int)(VFS_DIR_FIRST + k);
		}
	}

	return -1;
}

/* -------------------------------------------------------------------------- */
/* paths */

int vfs_resolve(const char *path, vfs_path_t *p)
{
	if (path == NULL || p == NULL) {
		return -1;
	}

	const char *s = path;
	uint8_t dir = (*s == '/') ? VFS_ROOT : vfs_cwd();

	/* ~ is the home, as in a Linux shell */
	if (s[0] == '~' && (s[1] == '\0' || s[1] == '/')) {
		vfs_path_t home;

		if (vfs_resolve(VFS_HOME, &home) < 0 || home.kind != VFS_DIR) {
			return -1;
		}

		dir = home.dir;
		s++;
	}

	p->name[0] = '\0';
	p->node = -1;

	for (;;) {
		while (*s == '/') {
			s++;
		}

		if (*s == '\0') {
			p->kind = VFS_DIR;
			p->dir = dir;
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

		int d = vfs_subdir(dir, p->name);

		if (d >= 0) {
			dir = (uint8_t)d;
			continue;
		}

		if (!last) {
			return -1;
		}

		int c = vfs_child(dir, p->name);

		p->dir = dir;
		p->kind = VFS_NONE;

		const vfs_fs_type_t *type;
		void *fs = vfs_mount_get(dir, &type);
		vfs_info_t t;

		if (c < 0 && fs != NULL && type->info(fs, p->name, &t) == 0) {
			p->kind = VFS_FILE;
			return 0;
		}

		/* a bare name: any plain node, from anywhere */
		bool bare = (strchr(path, '/') == NULL);

		for (int i = 0; c < 0 && bare && i < VFS_NODES_MAX; i++) {
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
		const char *name = (d == VFS_BIN) ? "bin" : (d == VFS_DEV) ? "dev" : vfs_dir(d)->name;

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
	return task_streams()->cwd;
}

int vfs_chdir(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_DIR) {
		return -1;
	}

	task_streams()->cwd = p.dir;

	return 0;
}

int vfs_dir_read(uint8_t dir, uint16_t *index, vfs_dirent_t *e)
{
	const uint16_t nodes = VFS_VIRTUAL + VFS_DIRS_MAX; /* where the nodes start */
	const uint16_t commands = nodes + VFS_NODES_MAX;
	const uint16_t files = commands + (uint16_t)vfs_commands_count;

	if (index == NULL || e == NULL || !vfs_dir_ok(dir)) {
		return -1;
	}

	memset(e, 0, sizeof(*e));
	e->node = -1;

	/* /bin and /dev, the directories, the nodes, the files of a file system */
	while (*index < files) {
		uint16_t i = (*index)++;

		if (i < VFS_VIRTUAL) {
			if (dir != VFS_ROOT) {
				continue;
			}

			strcpy(e->name, (i == 0) ? "bin" : "dev");
			e->kind = VFS_DIR;
			return 0;
		}

		if (i < nodes) {
			vfs_dir_t *d = &vfs_dirs[i - VFS_VIRTUAL];

			if (d->name[0] == '\0' || d->parent != dir) {
				continue;
			}

			strcpy(e->name, d->name);
			e->kind = VFS_DIR;
			e->mount = (d->fs != NULL);
			return 0;
		}

		if (i >= commands) {
			if (dir != VFS_BIN) {
				continue;
			}

			strcpy(e->name, vfs_commands[i - commands].name);
			e->kind = VFS_NODE;
			e->hidden = true;
			return 0;
		}

		int k = i - nodes;
		vfs_node_t *n = vfs_at(k);

		if (!vfs_used(k) || vfs_parent(n) != dir) {
			continue;
		}

		strcpy(e->name, vfs_name(n));
		e->kind = VFS_NODE;
		e->block = (n->handlers == &vfs_blk_handlers);
		e->hidden = n->hidden;
		e->node = (int16_t)k;

		int size = tmp_file_size(n); /* a RAM file has a size too */

		if (size > 0) {
			e->size = (uint32_t)size;
		}
		return 0;
	}

	const vfs_fs_type_t *type;
	void *fs = vfs_mount_get(dir, &type);
	uint16_t t = (uint16_t)(*index - files);
	vfs_info_t info;

	if (fs == NULL || type->dir(fs, &t, e->name, &info) < 0) {
		return -1;
	}

	*index = (uint16_t)(files + t);

	e->kind = VFS_FILE;
	e->size = info.size;
	e->hidden = (e->name[0] == '.'); /* as on Linux: ls -a shows it */

	return 0;
}

/* -------------------------------------------------------------------------- */
/* nodes, commands, directories */

int vfs_node_create(const char *path, bool hidden, void *args, const vfs_handler_t *handlers)
{
	vfs_path_t p;

	if (path == NULL || handlers == NULL) {
		return -1;
	}

	if (strchr(path, '/') == NULL) {
		return vfs_node_new(path, hidden, args, handlers);
	}

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_NONE) {
		return -1;
	}

	return vfs_node_add(p.dir, p.name, hidden, args, handlers);
}

/* a command's node: its args is the command */
static int vfs_app_run(void *args, int argc, char *argv[])
{
	int (*run)(void *, int, char *[]) = (int (*)(void *, int, char *[]))args;

	return run(NULL, argc, argv);
}

static const vfs_handler_t vfs_app_handlers = {
	.execute_handler = vfs_app_run,
};

int vfs_register_app(const char *path, int (*execute_handler)(void *args, int argc, char *argv[]))
{
	if (path == NULL || execute_handler == NULL) {
		return -1;
	}

	/* a bare name: a command in /bin, hidden as the built-in ones are */
	return vfs_node_create(path, strchr(path, '/') == NULL, (void *)execute_handler,
			       &vfs_app_handlers) < 0 ? -1 : 0;
}

int vfs_mkdir(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_NONE) {
		return -1;
	}

	return vfs_dir_add(p.dir, p.name) < 0 ? -1 : 0;
}

int vfs_remove(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0) {
		return -1;
	}

	if (p.kind == VFS_FILE) {
		const vfs_fs_type_t *type;
		void *fs = vfs_mount_get(p.dir, &type);

		return type->remove(fs, p.name);
	}

	if (p.kind == VFS_NODE && vfs_at(p.node)->handlers == &tmp_node) {
		/* a handle on it would write into whatever takes the node next */
		if (vfs_node_busy(p.node)) {
			return -1;
		}

		free(vfs_at(p.node)->args);
		vfs_drop(p.node);
		return 0;
	}

	vfs_dir_t *d = (p.kind == VFS_DIR) ? vfs_dir(p.dir) : NULL;

	if (d != NULL && vfs_dir_empty(p.dir)) {
		/* who was in it moves up, as nothing can be there any more */
		task_t *t = task_get_data();

		for (unsigned int i = 0; i < TASKS_MAX_COUNT; i++) {
			if (t[i].streams.cwd == p.dir) {
				t[i].streams.cwd = d->parent;
			}
		}

		if (vfs_cwd() == p.dir) {
			task_streams()->cwd = d->parent;
		}

		d->name[0] = '\0';
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
		const vfs_fs_type_t *type;
		void *fs = vfs_mount_get(dir, &type);

		if (dir != a.dir || b.kind == VFS_NODE || type->rename == NULL) {
			return -1; /* another file system: cp */
		}

		return type->rename(fs, a.name, name);
	}

	if (b.kind != VFS_DIR && b.kind != VFS_NONE) {
		return -1;
	}

	if (a.kind == VFS_NODE) {
		char full[VFS_NODE_NAME_MAX];

		/* a bare new name: a device stays a device, whatever the working directory */
		if (strchr(to, '/') == NULL && b.kind == VFS_NONE && !vfs_prefixed(vfs_at(a.node))) {
			dir = VFS_DEV;
		}

		if (vfs_fullname(dir, name, full) < 0) {
			return -1;
		}

		strcpy(vfs_at(a.node)->name, full);
		return 0;
	}

	vfs_dir_t *d = (a.kind == VFS_DIR) ? vfs_dir(a.dir) : NULL;

	if (d == NULL || dir == VFS_BIN || dir == VFS_DEV ||
	    !vfs_name_free(dir, name, VFS_DIR_NAME - 1u)) {
		return -1;
	}

	/* not into itself */
	for (uint8_t up = dir; up != VFS_ROOT; up = vfs_dir_parent(up)) {
		if (up == a.dir) {
			return -1;
		}
	}

	strcpy(d->name, name);
	d->parent = dir;

	return 0;
}

int vfs_tmp_create(const vfs_path_t *p)
{
	char full[VFS_NODE_NAME_MAX];

	if (p == NULL || p->kind != VFS_NONE || p->dir == VFS_BIN ||
	    vfs_fullname(p->dir, p->name, full) < 0) {
		return -1;
	}

	return tmp_file_create(full);
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

static int vfs_blk_lseek(void *args, off_t offset, int whence)
{
	vfs_blk_t *b = args;
	int32_t base = (whence == 0) ? 0 : (whence == 1) ? (int32_t)b->pos : (int32_t)b->size;

	if (base + (int32_t)offset < 0 || (uint32_t)(base + offset) > b->size) {
		return -1;
	}

	b->pos = (uint32_t)(base + offset);

	return (int)b->pos;
}

static const vfs_handler_t vfs_blk_handlers = {
	.read_handler = vfs_blk_read,
	.write_handler = vfs_blk_write,
	.lseek_handler = vfs_blk_lseek,
};

int vfs_blk_register(const char *name, vfs_blk_t *blk)
{
	if (name == NULL || blk == NULL || strchr(name, '/') != NULL) {
		return -1;
	}

	return (vfs_node_new(name, false, blk, &vfs_blk_handlers) < 0) ? -1 : 0;
}

const char *vfs_blk_name(const vfs_blk_t *blk)
{
	for (int i = 0; i < VFS_NODES_MAX; i++) {
		if (vfs_used(i) && vfs_at(i)->args == blk && vfs_at(i)->handlers == &vfs_blk_handlers) {
			return vfs_name(vfs_at(i));
		}
	}

	return "?";
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
/* handles: a table of what is open, devices and files alike */

typedef struct {
	const vfs_fs_type_t *type; /* a file: its file system's type; NULL: a node */
	void *fs;		   /* the file system */
	void *object;		   /* what the type's open() returned, or the node */
	unsigned int position;	   /* a node's read position */
	uint8_t holders;	   /* 0: a free entry */
	uint8_t flags;		   /* open()'s */
} vfs_fd_t;

static vfs_fd_t vfs_fds[VFS_FDS_MAX];
static int8_t vfs_fd_served = -1; /* the handle a node's handler is called for */

static vfs_fd_t *vfs_fd(int handle)
{
	int k = handle - VFS_FD_FIRST;

	return (k >= 0 && k < (int)VFS_FDS_MAX && vfs_fds[k].holders > 0) ? &vfs_fds[k] : NULL;
}

static int vfs_fd_new(const vfs_fs_type_t *type, void *fs, void *object, int flags)
{
	for (unsigned int k = 0; k < VFS_FDS_MAX; k++) {
		vfs_fd_t *f = &vfs_fds[k];

		if (f->holders == 0) {
			f->type = type;
			f->fs = fs;
			f->object = object;
			f->position = 0;
			f->holders = 1;
			f->flags = (uint8_t)(flags & (O_ACCMODE | O_APPEND));

			return VFS_FD_FIRST + (int)k;
		}
	}

	return -1;
}

static vfs_node_t *vfs_fd_serve(int handle)
{
	vfs_fd_t *f = vfs_fd(handle);

	if (f == NULL || f->type != NULL) {
		return NULL;
	}

	vfs_fd_served = (int8_t)handle;

	return f->object;
}

static bool vfs_node_busy(int node)
{
	for (unsigned int k = 0; k < VFS_FDS_MAX; k++) {
		if (vfs_fds[k].holders > 0 && vfs_fds[k].type == NULL &&
		    vfs_fds[k].object == vfs_at(node)) {
			return true;
		}
	}

	return false;
}

bool vfs_file_is(int handle)
{
	vfs_fd_t *f = vfs_fd(handle);

	return f != NULL && f->type != NULL;
}

vfs_node_t *vfs_fd_node(int handle)
{
	vfs_fd_t *f = vfs_fd(handle);

	return (f == NULL || f->type != NULL) ? NULL : f->object;
}

int vfs_fd_now(void)
{
	return vfs_fd_served;
}

unsigned int *vfs_position(void)
{
	return &vfs_fd(vfs_fd_served)->position;
}

int vfs_position_seek(unsigned int size, off_t offset, int whence)
{
	unsigned int *position = vfs_position();
	long to = (long)offset + ((whence == 0) ? 0L : (whence == 1) ? (long)*position : (long)size);

	if (to < 0 || to > (long)size) {
		return -1;
	}

	*position = (unsigned int)to;

	return (int)to;
}

int vfs_fd_read(int handle, void *buffer, unsigned int length)
{
	vfs_fd_t *f = vfs_fd(handle);

	if (f == NULL || buffer == NULL || (f->flags & O_ACCMODE) == O_WRONLY) {
		return -1;
	}

	if (f->type != NULL) {
		return f->type->read(f->object, buffer, length);
	}

	vfs_node_t *n = vfs_fd_serve(handle);

	return (n->handlers->read_handler == NULL) ? -1 :
						     n->handlers->read_handler(n->args, buffer, length);
}

int vfs_fd_write(int handle, const void *buffer, unsigned int length)
{
	vfs_fd_t *f = vfs_fd(handle);

	if (f == NULL || buffer == NULL || (f->flags & O_ACCMODE) == O_RDONLY) {
		return -1;
	}

	if (f->type != NULL) {
		return f->type->write(f->object, buffer, length);
	}

	vfs_node_t *n = vfs_fd_serve(handle);

	return (n->handlers->write_handler == NULL) ?
		       -1 :
		       n->handlers->write_handler(n->args, (void *)buffer, length);
}

int32_t vfs_fd_seek(int handle, int32_t offset, int whence)
{
	vfs_fd_t *f = vfs_fd(handle);

	if (f == NULL) {
		return -1;
	}

	if (f->type != NULL) {
		return (f->type->seek == NULL) ? -1 : f->type->seek(f->object, offset, whence);
	}

	vfs_node_t *n = vfs_fd_serve(handle);

	return (n->handlers->lseek_handler == NULL) ?
		       -1 :
		       n->handlers->lseek_handler(n->args, (off_t)offset, whence);
}

void vfs_fd_hold(int handle)
{
	vfs_fd_t *f = vfs_fd(handle);

	if (f != NULL) {
		f->holders++;
	}
}

int vfs_fd_close(int handle)
{
	vfs_fd_t *f = vfs_fd(handle);

	if (f == NULL) {
		return -1;
	}

	if (f->holders > 1) {
		f->holders--;
		return 0;
	}

	int r = 0;

	if (f->type != NULL) {
		r = f->type->close(f->object);
	} else {
		vfs_node_t *n = vfs_fd_serve(handle);

		if (n->handlers->close_handler != NULL) {
			r = n->handlers->close_handler(n->args);
		}
	}

	f->holders = 0;

	return r;
}

int vfs_file_abort(int handle)
{
	vfs_fd_t *f = vfs_fd(handle);

	if (f == NULL) {
		return -1;
	}

	int r = 0;

	if (f->type != NULL) {
		r = (f->type->abort != NULL) ? f->type->abort(f->object) : f->type->close(f->object);
	}

	f->holders = 0;

	return r;
}

int vfs_node_open(int node, int flags)
{
	vfs_node_t *n = vfs_at(node);
	int handle = vfs_fd_new(NULL, NULL, n, flags);

	if (handle >= 0 && n->handlers->open_handler != NULL) {
		vfs_fd_served = (int8_t)handle;
		n->handlers->open_handler(n->args, flags);
	}

	return handle;
}

int vfs_file_open(const vfs_path_t *p, int flags)
{
	const vfs_fs_type_t *type;
	void *fs = (p == NULL) ? NULL : vfs_mount_get(p->dir, &type);

	if (fs == NULL) {
		return -1;
	}

	/* a handle first: the type's open() can take a lot of memory and flash time */
	int handle = vfs_fd_new(type, fs, NULL, flags);

	if (handle < 0) {
		return -1;
	}

	void *file = type->open(fs, p->name, flags);

	if (file == NULL) {
		vfs_fd(handle)->holders = 0;
		return -1;
	}

	vfs_fd(handle)->object = file;

	return handle;
}

int vfs_file_info(const vfs_path_t *p, vfs_info_t *info)
{
	const vfs_fs_type_t *type;
	void *fs = (p == NULL || p->kind != VFS_FILE) ? NULL : vfs_mount_get(p->dir, &type);

	return (fs == NULL) ? -1 : type->info(fs, p->name, info);
}

/* -------------------------------------------------------------------------- */
/* mounts */

int vfs_fs_register(vfs_fs_type_t *type)
{
	if (type == NULL || type->name == NULL) {
		return -1;
	}

	for (vfs_fs_type_t *t = vfs_types; t != NULL; t = t->next) {
		if (t == type) {
			return 0;
		}
	}

	type->next = vfs_types;
	vfs_types = type;

	return 0;
}

void *vfs_mount_get(uint8_t dir, const vfs_fs_type_t **type)
{
	vfs_dir_t *d = vfs_dir(dir);

	if (d == NULL || d->fs == NULL) {
		return NULL;
	}

	if (type != NULL) {
		*type = d->type;
	}

	return d->fs;
}

int vfs_mount_type(const char *type, const char *source, const char *path, bool format)
{
	vfs_fs_type_t *t = vfs_types;
	vfs_path_t p;
	int d = -1;

	while (t != NULL && (type == NULL || strcmp(t->name, type) != 0)) {
		t = t->next;
	}

	if (t == NULL || source == NULL || vfs_resolve(path, &p) < 0) {
		return -1;
	}

	/* a source is mounted once (a device by its name or /dev path) */
	const char *base = strrchr(source, '/');

	base = (base == NULL) ? source : base + 1;

	for (unsigned int k = 0; k < VFS_DIRS_MAX; k++) {
		if (vfs_dirs[k].name[0] != '\0' && vfs_dirs[k].fs != NULL && vfs_dirs[k].type == t &&
		    strcmp(t->source(vfs_dirs[k].fs), base) == 0) {
			return -1;
		}
	}

	/* an empty directory, or a new one */
	if (p.kind == VFS_DIR && vfs_dir(p.dir) != NULL && vfs_dir_empty(p.dir)) {
		d = p.dir;
	}

	bool created = false;

	if (p.kind == VFS_NONE) {
		d = vfs_dir_add(p.dir, p.name);
		created = true;
	}

	if (d < 0) {
		return -1;
	}

	void *fs = t->mount(source, format);

	if (fs == NULL) {
		if (created) {
			vfs_dir((uint8_t)d)->name[0] = '\0';
		}

		return -1;
	}

	vfs_dir((uint8_t)d)->type = t;
	vfs_dir((uint8_t)d)->fs = fs;

	return 0;
}

int vfs_umount(const char *path)
{
	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind != VFS_DIR) {
		return -1;
	}

	vfs_dir_t *d = vfs_dir(p.dir);

	if (d == NULL || d->fs == NULL) {
		return -1;
	}

	for (unsigned int k = 0; k < VFS_FDS_MAX; k++) {
		if (vfs_fds[k].holders > 0 && vfs_fds[k].fs == d->fs) {
			return -1; /* a file is open */
		}
	}

	d->type->umount(d->fs);
	d->fs = NULL;
	d->type = NULL;

	return 0;
}

int vfs_boot(const char *type, const char *dev)
{
	static const char rc[] = VFS_BOOT_RC;
	vfs_path_t p;

	/* format: a new device (or one holding something else) gets an empty file system */
	if (vfs_mount_type(type, dev, "/boot", true) < 0) {
		/* no /boot: the recovery console, line by line */
		char line[32];

		for (const char *s = rc; *s != '\0';) {
			size_t n = strcspn(s, "\n");

			if (n < sizeof(line)) {
				memcpy(line, s, n);
				line[n] = '\0';
				system(line);
			}

			s += n + (s[n] == '\n');
		}

		return -1;
	}

	/* no rc (a new board, a deleted rc): the recovery rc; an rc that is there stays */
	int fd = open("/boot/rc", O_WRONLY | O_CREAT | O_EXCL, 0);

	if (fd >= 0) {
		write(fd, (void *)rc, strlen(rc));
		close(fd);
	}

	return (vfs_resolve("/boot/rc", &p) == 0 && p.kind == VFS_FILE) ? 0 : -1;
}

/* -------------------------------------------------------------------------- */

int vfs_initialize(void)
{
	vfs_nodes_clear();
	task_streams()->cwd = VFS_ROOT;
	memset(vfs_dirs, 0, sizeof(vfs_dirs));

	if (vfs_mkdir("/opt") < 0 || vfs_mkdir("/tmp") < 0 || vfs_mkdir("/mnt") < 0) {
		return -1;
	}

#ifndef VFS_NO_TFS
	vfs_fs_register(&vfs_tfs_type);
#endif

	return 0;
}
