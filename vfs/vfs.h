/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file vfs.h
 * @brief Virtual file system: directories, paths and mounted file systems on the fsi node table.
 *
 * vfs is fsi with a tree. It uses the same node table (fsi.c, unchanged) and
 * keeps everything fsi does: fsi_node_create(), fsi_register_app(), bare names
 * such as `ttyS1` and `std ttyS1` work as before, and a driver or an app
 * written for fsi runs on vfs unchanged. On top it adds:
 *
 *   /bin    the commands: every node with an execute handler (fsi_register_app)
 *   /dev    the devices: every other plain fsi node (ttyS1, ain1, ee0, ...)
 *   /app    project applications (vfs_register_app("/app/trgmon", ...))
 *   /tmp    RAM files (touch), or a file system mounted over it
 *   /mnt    mount points of file systems on SPI EEPROM / SRAM (mount ee0 /mnt/ee)
 *
 * and paths everywhere: open("/mnt/ee/config", O_WRONLY | O_CREAT | O_TRUNC),
 * `cat /mnt/ee/boot.as`, `as /mnt/ee/boot.as`, `cd /tmp`, `ls -l /dev`.
 *
 * How it fits in the fsi table without new fields: /bin and /dev are views of
 * the plain nodes (no directory entries needed); a node in any other
 * directory carries that directory in the first byte of its name (0x80 | the
 * directory's index, so such names have 14 characters). An open file is a
 * hidden node that lives until close().
 *
 * A project uses vfs/posix.c instead of kernel/posix.c (the C library calls
 * with path support) and boots:
 * @code
 * fsi_initialize();
 * vfs_initialize();                        // /app, /tmp, /mnt
 * board_initialize();                      // devices, vfs_blk_register("ee0", ...)
 * shell_initialize(startup);
 * vfs_bin_initialize();                    // path-aware ls, cat, cp, mount, ...
 * @endcode
 */

#ifndef VFS_H
#define VFS_H

#include "../fsi/fsi.h"
#include "blk.h"
#include "tfs.h"

#include <stdint.h>
#include <stdbool.h>

/* open() flags; XC16 has no <fcntl.h>, so the values of its <bits/fcntl.h> */
#ifndef O_WRONLY
#define O_RDONLY 0
#define O_WRONLY 01
#define O_RDWR	 02
#endif
#ifndef O_CREAT
#define O_CREAT	 0400
#define O_EXCL	 02000
#define O_TRUNC	 01000
#define O_APPEND 0010
#endif

#define VFS_ROOT 0u  /**< Directory id of /. */
#define VFS_BIN	 1u  /**< Directory id of /bin. */
#define VFS_DEV	 2u  /**< Directory id of /dev. */
#define VFS_PATH_MAX 64u /**< Longest path printed by pwd / df. */

/** What a path names. */
enum VFS_KIND {
	VFS_NONE = 0, /**< Nothing (yet): `dir` is where it would be created. */
	VFS_DIR = 1,  /**< A directory or a mount point: `dir`. */
	VFS_NODE = 2, /**< A node of the table: `node`. */
	VFS_FILE = 3, /**< A file of the file system mounted at `dir`. */
};

/** A resolved path. */
typedef struct {
	uint8_t kind;		  /**< enum VFS_KIND. */
	uint8_t dir;		  /**< Directory id: VFS_ROOT, _BIN, _DEV or a node. */
	int16_t node;		  /**< Node index for VFS_NODE, else -1. */
	char name[TFS_NAME_SIZE]; /**< The last part of the path. */
} vfs_path_t;

/** One directory entry, from vfs_dir_read(). */
typedef struct {
	char name[TFS_NAME_SIZE]; /**< Name. */
	uint8_t kind;		  /**< VFS_DIR, VFS_NODE or VFS_FILE. */
	bool mount;		  /**< A mount point (kind VFS_DIR). */
	bool hidden;		  /**< A hidden node. */
	int16_t node;		  /**< Node index (node or table directory), or -1. */
	uint32_t size;		  /**< File size. */
} vfs_dirent_t;

/** @brief Creates /app, /tmp and /mnt and sets the working directory to /. @return 0 or -1. */
int vfs_initialize(void);

/**
 * @brief Resolves @p path, relative to the working directory unless it starts with '/'.
 *
 * A bare name (no '/') not found in the working directory is looked up among
 * all plain fsi nodes, exactly like fsi: `ttyS1` works from anywhere.
 *
 * @return 0, or -1 when a directory on the way does not exist.
 */
int vfs_resolve(const char *path, vfs_path_t *p);

/**
 * @brief Adds a node at @p path (like fsi_node_create(), with a directory).
 *
 * In /bin and /dev, and for a bare name, it is a plain fsi node (in /bin when
 * it has an execute handler, else in /dev).
 *
 * @return The node index, or -1.
 */
int vfs_node_create(const char *path, bool hidden, void *args, fsi_handler_t *handlers);

/**
 * @brief Registers a command at @p path (`/app/trgmon`); a bare name goes to /bin.
 * @return 0 or -1.
 */
int vfs_register_app(const char *path, int (*execute_handler)(void *args, int argc, char *argv[]));

/**
 * @brief Sets the handler of the command @p name in /bin, registering it if it is not there.
 *
 * Used to replace a kernel command (ls, cat, ...) by its vfs version.
 *
 * @return 0 or -1.
 */
int vfs_app_set(const char *name, int (*execute_handler)(void *args, int argc, char *argv[]));

/** @brief Makes block device @p blk the node /dev/@p name. @return 0 or -1. */
int vfs_blk_register(const char *name, vfs_blk_t *blk);

/** @brief Creates a directory. @return 0 or -1. */
int vfs_mkdir(const char *path);

/**
 * @brief Removes a file, a RAM file (touch) or an empty directory.
 * @return 0, or -1 (devices and commands are not removed).
 */
int vfs_remove(const char *path);

/** @brief Renames or moves within one file system or within the node table. @return 0 or -1. */
int vfs_rename(const char *from, const char *to);

/** @brief Changes the working directory. @return 0 or -1. */
int vfs_chdir(const char *path);

/** @brief Writes the path of directory @p dir into @p buffer. @return 0 or -1. */
int vfs_dir_path(uint8_t dir, char *buffer, unsigned int size);

/** @brief Returns the working directory's id. */
uint8_t vfs_cwd(void);

/**
 * @brief Lists directory @p dir: call with *index = 0, then again while it returns 0.
 * @return 0 with the next entry in @p e, -1 after the last.
 */
int vfs_dir_read(uint8_t dir, uint16_t *index, vfs_dirent_t *e);

/** @brief Returns the file system mounted at directory @p dir, or NULL. */
tfs_t *vfs_mount_get(uint8_t dir);

/** @brief Returns the block device at @p path (`ee0` or `/dev/ee0`), or NULL. */
vfs_blk_t *vfs_blk_get(const char *path);

/**
 * @brief Mounts the file system on device @p dev at @p path.
 *
 * @p path is created when it does not exist; an existing one must be an empty
 * directory.
 *
 * @param format true: format the device first if it holds no file system
 *               (an SPI SRAM after power-up).
 * @return 0 or -1.
 */
int vfs_mount(const char *dev, const char *path, bool format);

/** @brief Unmounts @p path (no file may be open on it). @return 0 or -1. */
int vfs_umount(const char *path);

/**
 * @brief Opens a file of a mounted file system as a node.
 *
 * Used by open(). Flags 0 opens like an fsi RAM file: reads from the start,
 * writes append. O_WRONLY / O_RDWR write at the position; O_CREAT, O_EXCL,
 * O_TRUNC (atomic replace at close) and O_APPEND as POSIX.
 *
 * @return The handle (node index), or -1.
 */
int vfs_file_open(const vfs_path_t *p, int flags);

/**
 * @brief Creates a RAM file (kernel `touch`, 126 bytes) where @p p, a VFS_NONE path, points.
 * @return Its node index, or -1.
 */
int vfs_tmp_create(const vfs_path_t *p);

/** @brief Returns the open file behind @p handle, or NULL when it is not a file. */
tfs_file_t *vfs_file_get(int handle);

/** @brief Registers the vfs commands (call after shell_initialize()). @return 0. */
int vfs_bin_initialize(void);

#endif /* VFS_H */
