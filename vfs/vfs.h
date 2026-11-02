/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file vfs.h
 * @brief Virtual file system: the node table, directories, paths and mounted file systems.
 *
 * Every device, command and file of the system is a node of one table
 * (node.c): a driver creates its node with vfs_node_create(), an application
 * registers a command with vfs_register_app(), and the C library calls
 * (posix.c: open, read, write, close, lseek, system) reach them by name or
 * by path. Bare names such as `ttyS1` and `std ttyS1` work from anywhere. The
 * tree:
 *
 *   /bin    the commands: every node that only executes (vfs_register_app)
 *   /dev    the devices: every plain node that can be read or written,
 *           also when it takes commands too (ttyS1, a GPIO, ain1, ee0, ...)
 *   /opt    project applications (vfs_register_app("/opt/trgmon", ...))
 *   /tmp    RAM files (touch), or a file system mounted over it
 *   /mnt    mount points of file systems on SPI EEPROM / SRAM (mount ee0 /mnt/ee)
 *
 * and paths everywhere: open("/mnt/ee/config", O_WRONLY | O_CREAT | O_TRUNC),
 * `cat /mnt/ee/boot.as`, `as /mnt/ee/boot.as`, `cd /tmp`, `ls -l /dev`.
 *
 * How the tree fits the table without extra fields: /bin and /dev are views of
 * the plain nodes; a node in any other directory carries that directory in
 * the first byte of its name (0x80 | the directory's id, so such names have
 * 14 characters). Directories and mount points are in a table of their own
 * (VFS_DIRS_MAX), and handles in a table of open files (VFS_FDS_MAX), devices
 * and files alike, so neither takes a node. The built-in commands are in flash
 * (vfs_commands), listed in /bin, and take no node either.
 *
 * File systems are mounted by type, as on Linux (`mount -t drn router /drn`):
 * a type is a vfs_fs_type_t registered with vfs_fs_register(). tfs (SPI
 * memories, flash) is built in; the DRN router file system is another.
 *
 * Build options: VFS_NODES_MAX sets the size of the node table (24),
 * VFS_FDS_MAX the handles (8);
 * VFS_NO_TFS leaves out tfs (and mkfs, crc) for a small MCU with no memory
 * to keep files on; mounts of other types still work; VFS_NO_REDIRECT leaves
 * out `<`, `>`, `>>` and the `shell` command for a full flash.
 *
 * A project links vfs/node.c, vfs.c, posix.c (the C library calls with path
 * support), bin.c (the shell commands) and, without VFS_NO_TFS, tfs.c and
 * tfsvfs.c, and boots:
 * @code
 * vfs_initialize();                        // the node table, /opt, /tmp, /mnt
 * board_initialize();                      // devices, vfs_blk_register("ee0", ...)
 * shell_initialize();
 * vfs_bin_initialize();                    // path-aware ls, cat, cp, mount, ...
 * vfs_interpreter_set(argscript_execute);  // runs script files
 * system("/boot/rc");                      // the board's startup
 * @endcode
 */

#ifndef VFS_H
#define VFS_H

#include "blk.h"
#include "tfs.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* the node table (node.c) */

#ifndef VFS_NODES_MAX
#define VFS_NODES_MAX 24 /**< Size of the node table: devices, RAM files, project commands. */
#endif
#define VFS_NODE_NAME_MAX 16 /**< Node name length, including the 0. */

#ifndef VFS_BOOT_RC
/** The rc of a board that has none (vfs_boot()): a console to write its rc from. */
#define VFS_BOOT_RC "ttyS1 -b 9600\nstd ttyS1\n"
#endif

/** What a node can do. Any handler may be NULL. Each gets the node's args. */
typedef struct {
	/** Called by open(); the result is ignored. */
	int (*open_handler)(void *args, int flags);
	/** Writes @p length bytes; returns the bytes written or -1. */
	int (*write_handler)(void *args, void *buffer, unsigned int length);
	/** Reads up to @p length bytes; returns the bytes read or -1. */
	int (*read_handler)(void *args, void *buffer, unsigned int length);
	/** Moves the read position, as lseek(). */
	int (*lseek_handler)(void *args, off_t offset, int whence);
	/** Called by close() of the last handle on the node. */
	int (*close_handler)(void *args);
	/** Runs the node as a command (see system()). */
	int (*execute_handler)(void *args, int argc, char *argv[]);
} vfs_handler_t;

/** One entry of the node table. */
typedef struct {
	char name[VFS_NODE_NAME_MAX];	/**< Name; name[0] & 0x80: in a vfs directory. */
	void *args;			/**< Passed to the handlers. */
	const vfs_handler_t *handlers;	/**< What the node can do; NULL: a free entry. */
	bool hidden;			/**< Not listed by plain `ls`. */
} vfs_node_t;

/** A command in flash: it takes no RAM, and is in /bin. */
typedef struct {
	const char *name;
	int (*run)(void *args, int argc, char *argv[]);
} vfs_command_t;

extern const vfs_command_t vfs_commands[];     /**< The built-in commands (bin.c). */
extern const unsigned int vfs_commands_count;  /**< How many. */

/** @brief Returns the node table (VFS_NODES_MAX entries). */
vfs_node_t *vfs_nodes_get(void);

/**
 * @brief Puts a node in the first free entry.
 *
 * The raw table call under vfs_node_create(): @p name is stored as it is.
 * @return The node's index, or -1 when the table is full.
 */
int vfs_node_new(const char *name, bool hidden, void *args, const vfs_handler_t *handlers);

/** @brief The `std` command: std [-t] PATH makes a node or a file (a DRN port) the console. */
int vfs_std_execute(void *args, int argc, char *argv[]);

/** @brief The handle of the console, -1 when there is none. */
int vfs_console(void);

/** @brief true when the running task's streams are a terminal (`shell -t`, `std -t`). */
bool vfs_terminal(void);

/** @brief The handle behind 0, 1 or 2 for the running task: its own, else the console's, or -1. */
int vfs_stream(int handle);

/** @brief Passes on a line stdout still holds. */
void vfs_stdout_flush(void);

/* -------------------------------------------------------------------------- */
/* the tree */

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
#ifndef O_ACCMODE
#define O_ACCMODE 03
#endif

#define VFS_ROOT 0u  /**< Directory id of /. */
#define VFS_BIN	 1u  /**< Directory id of /bin. */
#define VFS_DEV	 2u  /**< Directory id of /dev. */
#define VFS_PATH_MAX 64u /**< Longest path printed by pwd / df. */
#ifndef VFS_DIRS_MAX
#ifdef VFS_NO_TFS
#define VFS_DIRS_MAX 6u /**< Directories and mount points besides /, /bin and /dev. */
#else
#define VFS_DIRS_MAX 8u /**< Directories and mount points besides /, /bin and /dev. */
#endif
#endif
#define VFS_DIR_NAME  12u /**< Directory name length, including the 0. */
#define VFS_DIR_FIRST 3u  /**< Ids of the directories: VFS_DIR_FIRST .. + VFS_DIRS_MAX - 1. */
#ifndef VFS_FDS_MAX
#define VFS_FDS_MAX 8u /**< Handles open at the same time, devices and files: a project can set its own. */
#endif
#define VFS_FD_FIRST 3 /**< The first handle: 0, 1 and 2 are the task's streams. */
#ifndef VFS_HOME
#define VFS_HOME "/home" /**< `cd` with no argument, `~` in the prompt, scripts run by name. */
#endif

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
	uint8_t dir;		  /**< Directory id: VFS_ROOT, _BIN, _DEV or VFS_DIR_FIRST + k. */
	int16_t node;		  /**< Node index for VFS_NODE, else -1. */
	char name[TFS_NAME_SIZE]; /**< The last part of the path. */
} vfs_path_t;

/** One directory entry, from vfs_dir_read(). */
typedef struct {
	char name[TFS_NAME_SIZE]; /**< Name. */
	uint8_t kind;		  /**< VFS_DIR, VFS_NODE or VFS_FILE. */
	bool mount;		  /**< A mount point (kind VFS_DIR). */
	bool hidden;		  /**< A hidden node, or a file whose name starts with '.'. */
	bool block;		  /**< A block device (vfs_blk_register()). */
	int16_t node;		  /**< Node index for VFS_NODE, else -1 (and -1 for a built-in command). */
	uint32_t size;		  /**< File size. */
} vfs_dirent_t;

/** @brief Empties the node table, registers tfs, creates /opt, /tmp and /mnt, sets the working directory to /. @return 0 or -1. */
int vfs_initialize(void);

/**
 * @brief Resolves @p path, relative to the working directory unless it starts with '/'.
 *
 * A bare name (no '/') not found in the working directory is looked up among
 * all plain nodes: `ttyS1` works from anywhere.
 *
 * @return 0, or -1 when a directory on the way does not exist.
 */
int vfs_resolve(const char *path, vfs_path_t *p);

/**
 * @brief Adds a node at @p path: how a driver makes its device.
 *
 * In /bin and /dev, and for a bare name, it is a plain node (in /dev when
 * it has a read or a write handler, else in /bin).
 *
 * @return The node index, or -1.
 */
int vfs_node_create(const char *path, bool hidden, void *args, const vfs_handler_t *handlers);

/**
 * @brief Registers a command at @p path; a bare name goes to /bin.
 *
 * /bin is for the system's commands; an application registers in /opt
 * (`/opt/trg`). Either runs by its bare name, before a built-in of that name.
 * It takes a node of the table and no heap.
 * @return 0 or -1.
 */
int vfs_register_app(const char *path, int (*execute_handler)(void *args, int argc, char *argv[]));

/** @brief Makes block device @p blk the node /dev/@p name. @return 0 or -1. */
int vfs_blk_register(const char *name, vfs_blk_t *blk);

/** @brief Creates a directory. @return 0 or -1. */
int vfs_mkdir(const char *path);

/**
 * @brief Removes a file, a RAM file (touch) or an empty directory.
 * @return 0, or -1 (devices and commands are not removed, nor a RAM file that is open).
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

/* -------------------------------------------------------------------------- */
/* file systems */

/** What a file system says about one of its files. */
typedef struct {
	uint32_t size;	/**< Size in bytes. */
	uint32_t crc;	/**< CRC-32 of the whole file, when has_crc. */
	bool has_crc;	/**< The file system keeps the file's CRC (tfs). */
} vfs_info_t;

/**
 * A kind of file system, as Linux's file_system_type: `mount -t NAME SOURCE PATH`.
 *
 * A file system names its files with plain names (no '/'). The ops marked
 * optional may be NULL. Flags are open()'s (O_WRONLY, O_CREAT, ...).
 */
typedef struct vfs_fs_type {
	const char *name; /**< `mount -t` name: "tfs", "drn". */
	/** Mounts @p source (a device, or whatever the type understands); the file system or NULL. */
	void *(*mount)(const char *source, bool format);
	/** Releases what mount() took; the files are closed. */
	void (*umount)(void *fs);
	/** 0 and @p info when file @p name exists, else -1. */
	int (*info)(void *fs, const char *name, vfs_info_t *info);
	/** The next file from *@p index (0 first): 0 with its name, -1 after the last. */
	int (*dir)(void *fs, uint16_t *index, char *name, vfs_info_t *info);
	/** Opens (O_CREAT: creates) file @p name; the open file or NULL. */
	void *(*open)(void *fs, const char *name, int flags);
	/** Reads up to @p length bytes: the bytes read, 0 for none (yet), -1. */
	int (*read)(void *file, void *buffer, unsigned int length);
	/** Writes @p length bytes: the bytes taken, 0 when it can't now (try later), -1. */
	int (*write)(void *file, const void *buffer, unsigned int length);
	/** Optional: moves the position, as lseek(); the new position or -1. */
	int32_t (*seek)(void *file, int32_t offset, int whence);
	/** Commits and frees the open file; 0 or -1. */
	int (*close)(void *file);
	/** Optional: frees the open file without committing it, the old one stays; 0 or -1. */
	int (*abort)(void *file);
	/** Removes file @p name; 0 or -1. */
	int (*remove)(void *fs, const char *name);
	/** Optional: renames a file; 0 or -1. */
	int (*rename)(void *fs, const char *from, const char *to);
	/** Optional: size and free bytes, for df; 0 or -1. */
	int (*usage)(void *fs, uint32_t *size, uint32_t *avail);
	/** What was mounted, for df and mount: the device's name, "router", ... */
	const char *(*source)(void *fs);
	struct vfs_fs_type *next; /**< Set by vfs_fs_register(). */
} vfs_fs_type_t;

/** @brief Makes a kind of file system mountable (`mount -t`). @return 0 or -1. */
int vfs_fs_register(vfs_fs_type_t *type);

/**
 * @brief Mounts @p source with the file system type named @p type at @p path.
 *
 * @p path is created when it does not exist; an existing one must be an empty
 * directory. @p format is passed to the type (tfs: format a device that
 * holds no file system).
 *
 * @return 0 or -1.
 */
int vfs_mount_type(const char *type, const char *source, const char *path, bool format);

/**
 * @brief Mounts the tfs on block device @p dev at @p path (vfs_mount_type("tfs", ...)).
 * @param format true: format the device first if it holds no file system
 *               (an SPI SRAM after power-up).
 * @return 0 or -1.
 */
int vfs_mount(const char *dev, const char *path, bool format);

/** @brief Unmounts @p path (no file may be open on it). @return 0 or -1. */
int vfs_umount(const char *path);

/** @brief The file system mounted at directory @p dir and its type, or NULL. */
void *vfs_mount_get(uint8_t dir, const vfs_fs_type_t **type);

/** @brief 0 and @p info when @p p (a VFS_FILE path) names a file, else -1. */
int vfs_file_info(const vfs_path_t *p, vfs_info_t *info);

/** @brief Returns the block device at @p path (`ee0` or `/dev/ee0`), or NULL. */
vfs_blk_t *vfs_blk_get(const char *path);

/** @brief The name block device @p blk is registered under, or "?". */
const char *vfs_blk_name(const vfs_blk_t *blk);

/**
 * @brief Mounts block device @p dev with file system @p type at /boot and makes sure /boot/rc is there.
 *
 * Every board boots the same way: its board code registers the device that
 * holds /boot (program flash, an EEPROM, an SPI flash), then
 * @code
 * vfs_boot("tfs", "flash0");
 * ...
 * system("/boot/rc");                      // after the other inits, with argscript
 * @endcode
 * A device with no file system is formatted (mount with format). When there
 * is no /boot/rc (a new board, a deleted rc) it gets VFS_BOOT_RC, the same on
 * every board: a console on ttyS1 to write the board's rc from. An rc that is
 * there stays as it is. When /boot can't be mounted, VFS_BOOT_RC is run at
 * once. "tfs" for a memory of 1 KiB and more, "one" (vfs/onefs.c: one file,
 * rc) for a smaller one.
 *
 * @return 0 when /boot/rc is there, else -1.
 */
int vfs_boot(const char *type, const char *dev);

/**
 * @brief Opens a file of a mounted file system.
 *
 * Used by open(). On tfs, O_CREAT, O_EXCL, O_TRUNC (atomic replace at close) and
 * O_APPEND work as in POSIX, O_WRONLY and O_RDWR write at the position.
 *
 * @return The handle, or -1.
 */
int vfs_file_open(const vfs_path_t *p, int flags);

/** @brief A handle on node @p node, with its open handler called. @return The handle, or -1. */
int vfs_node_open(int node, int flags);

/** @brief The node behind @p handle, NULL when it is a file of a file system or not open. */
vfs_node_t *vfs_fd_node(int handle);

/** @brief One more holder of @p handle: close() has to be called once more. */
void vfs_fd_hold(int handle);

/** @brief Lets go of @p handle; the last close commits a file, or calls the node's close handler. @return 0 or -1. */
int vfs_fd_close(int handle);

/**
 * @brief Gives @p handle up without finishing it: a file keeps what it had before,
 * a node's close handler is not called. @return 0 or -1.
 */
int vfs_file_abort(int handle);

/** @brief true when @p handle is an open file of a file system (not a node). */
bool vfs_file_is(int handle);

/** @brief read() / write() / lseek() on a handle that is not a stream. */
int vfs_fd_read(int handle, void *buffer, unsigned int length);
int vfs_fd_write(int handle, const void *buffer, unsigned int length);
int32_t vfs_fd_seek(int handle, int32_t offset, int whence);

/** @brief For a node's handlers: the handle being served. */
int vfs_fd_now(void);

/** @brief For a node's handlers: the read position of the handle being served. */
unsigned int *vfs_position(void);

/** @brief For a node's lseek handler: moves the position in a node of @p size bytes. */
int vfs_position_seek(unsigned int size, off_t offset, int whence);

/**
 * "one" (vfs/onefs.c): a block device holding one file, rc, for a memory too
 * small for tfs (an I2C EEPROM of 256 bytes). Not registered by default:
 * vfs_fs_register(&vfs_one_type), then vfs_boot("one", "ee0").
 */
extern vfs_fs_type_t vfs_one_type;

#ifndef VFS_NO_TFS
/** The tfs as a file system type (vfs/tfsvfs.c), registered by vfs_initialize(). */
extern vfs_fs_type_t vfs_tfs_type;

/** @brief The `mkfs dev [block] [entries]` and `crc file` commands (vfs/tfsvfs.c). */
int vfs_tfs_mkfs(void *args, int argc, char *argv[]);
int vfs_tfs_crc(void *args, int argc, char *argv[]);
#endif

/**
 * @brief Creates a RAM file (126 bytes) where @p p, a VFS_NONE path, points.
 * @return Its node index, or -1.
 */
int vfs_tmp_create(const vfs_path_t *p);

/**
 * @brief Sets what runs a script typed as a command, like `#!` on Linux.
 *
 * A command that is a file (`/home/blink`, `./blink`, a RAM file) or the
 * name of a file in VFS_HOME (`blink`) is run as
 * run(NULL, 3, {"exec", path, name}). With argscript:
 * @code
 * vfs_interpreter_set(argscript_execute);   // `blink` runs /home/blink as task "blink"
 * @endcode
 * The words after the name are not passed. NULL (the default): a script
 * file does not run.
 */
void vfs_interpreter_set(int (*run)(void *args, int argc, char *argv[]));

/**
 * @brief The `ed` command (vfs/ed.c, optional): edits a file one line per command.
 *
 * @code
 * vfs_register_app("ed", vfs_ed);
 * @endcode
 * ed FILE lists it numbered; ed FILE Nc TEXT changes line N, Na adds after
 * it (0a: first), Ni inserts before it, Nd deletes it. The whole file is in
 * RAM during the command only (up to ED_FILE_MAX, 1024 bytes) and is written
 * back at once with O_TRUNC.
 */
int vfs_ed(void *args, int argc, char *argv[]);

/** @brief Registers the vfs commands (call after shell_initialize()). @return 0. */
int vfs_bin_initialize(void);

#endif /* VFS_H */
