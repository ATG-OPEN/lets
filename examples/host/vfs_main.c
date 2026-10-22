/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The kernel with the virtual file system on a PC. Like main.c, plus two RAM disks standing in
 * for a board's SPI EEPROM and SPI SRAM: /tmp is a file system on ram1 (formatted at boot, as
 * an SRAM would be), /mnt/ee one on ee0. Try: ls /, ls -l /dev, cd /mnt/ee, echo hi > f,
 * cat f, cp f /tmp, crc f, df, pwd.
 */
#include "../../kernel/task.h"
#include "../../kernel/alloc.h"
#include "../../kernel/bin/shell.h"
#include "../../fsi/fsi.h"
#include "../../vfs/vfs.h"
#include "../../vfs/dev/ramdisk.h"
#include "../../port/host/host_tty.h"

ALLOC_INIT_HEAP(16384);

static uint64_t system_time_get(void)
{
	return (uint64_t)host_time_us();
}

static fsi_handler_t tty = {
	.read_handler = host_tty_read,
	.write_handler = host_tty_write,
};

static uint8_t ee0_mem[32768];
static uint8_t ram1_mem[8192];
static ramdisk_t ee0, ram1;

static int idle(void *args, uint64_t system_time)
{
	(void)args;
	(void)system_time;
	host_idle(1000);
	return 0;
}

static char startup[] = "std tty\n"
			"mount -f ram1 /tmp\n"
			"mkfs ee0\n"
			"mount ee0 /mnt/ee\n"
			"echo vfs on the host - try: ls /, ls -l /dev, cd /mnt/ee, df\n";

int main(void)
{
	host_tty_setup();
	setvbuf(stdout, NULL, _IONBF, 0); /* printf and the shell's writes in order */

	task_initialize(system_time_get);
	fsi_initialize();
	vfs_initialize();

	/* the board */
	fsi_node_create("tty", false, NULL, &tty);
	ramdisk_init(&ee0, ee0_mem, sizeof(ee0_mem));
	ramdisk_init(&ram1, ram1_mem, sizeof(ram1_mem));
	vfs_blk_register("ee0", &ee0.blk);
	vfs_blk_register("ram1", &ram1.blk);

	shell_initialize((void *)startup);
	vfs_bin_initialize();
	task_create("idle", idle, NULL);

	tasks_start(); /* never returns */
	return 0;
}
