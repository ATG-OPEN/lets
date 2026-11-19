# PC build of the kernel (demo + tests). The MCU build uses your MPLAB X / XC16 project: add the
# kernel, vfs and lib sources to it (see docs/porting.md).
#
#   make          the host demos: build/lets-demo (a shell on your terminal, VFS_NO_TFS) and
#                 build/lets-vfs-demo (the same with file systems)
#   make test     every host test
#   make format   applies the code style (clang-format)
#   make docs     API reference (Doxygen) in build/docs/html

CC      ?= cc
WARN    = -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS  ?= -std=gnu11 -O1 -g $(WARN)
SAN     = -fsanitize=address,undefined
PRELUDE = -include port/host/prelude.h -Iinclude

# the kernel, its shell and vfs without tfs (VFS_NO_TFS, as on a small MCU) ...
KERNEL  = kernel/task.c kernel/clock.c kernel/alloc.c lib/args.c $(wildcard bin/*.c) \
          vfs/node.c vfs/vfs.c vfs/posix.c vfs/tmp.c vfs/proc.c vfs/sys.c vfs/onefs.c
NOFS    = -DVFS_NO_TFS

# ... and with tfs and the memory drivers
VFS     = $(KERNEL) vfs/tfs.c vfs/tfsvfs.c $(filter-out vfs/dev/flash24.c,$(wildcard vfs/dev/*.c))

all: build/lets-demo build/lets-vfs-demo

build:
	mkdir -p build

build/host_tty.o: port/host/host_tty.c port/host/host_tty.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/lets-demo: examples/host/main.c $(KERNEL) build/host_tty.o | build
	$(CC) $(CFLAGS) $(NOFS) $(PRELUDE) examples/host/main.c $(KERNEL) build/host_tty.o -o $@

build/lets-vfs-demo: examples/host/vfs_main.c $(VFS) build/host_tty.o | build
	$(CC) $(CFLAGS) $(PRELUDE) examples/host/vfs_main.c $(VFS) build/host_tty.o -o $@

# flash24 is the PIC24FJ's own flash: on a PC only the test, which emulates it
build/test_vfs: tests/test_vfs.c $(VFS) vfs/dev/flash24.c | build
	$(CC) $(CFLAGS) $(SAN) $(PRELUDE) tests/test_vfs.c $(VFS) vfs/dev/flash24.c -o $@

build/test_kernel: tests/test_kernel.c $(KERNEL) | build
	$(CC) $(CFLAGS) $(NOFS) $(SAN) $(PRELUDE) tests/test_kernel.c $(KERNEL) -o $@

build/test_sleep: tests/test_sleep.c $(KERNEL) | build
	$(CC) $(CFLAGS) $(NOFS) $(SAN) $(PRELUDE) tests/test_sleep.c $(KERNEL) -o $@

# the POSIX line: /proc, kill(), reboot(), top; and the same without the /proc mount
build/test_proc: tests/test_proc.c $(KERNEL) | build
	$(CC) $(CFLAGS) $(NOFS) $(SAN) $(PRELUDE) tests/test_proc.c $(KERNEL) -o $@

build/test_proc_fd: tests/test_proc.c $(KERNEL) | build
	$(CC) $(CFLAGS) $(NOFS) $(SAN) $(PRELUDE) -DVFS_FD_TASK=1 tests/test_proc.c $(KERNEL) -o $@

# the scheduler's watchdog (LETS_WDT) on a fake clock and a fake watchdog
build/test_wdt: tests/test_wdt.c $(KERNEL) | build
	$(CC) $(CFLAGS) $(NOFS) $(SAN) $(PRELUDE) -DLETS_WDT=1 tests/test_wdt.c $(KERNEL) -o $@

build/as_test: argscript/test/as_test.c argscript/argscript.c lib/args.c | build
	$(CC) -std=c99 $(WARN) $(SAN) -include fcntl.h -Dopen=t_open -Dread=t_read -Dwrite=t_write \
	    -Dclose=t_close -Dsystem=t_system -Dlseek=t_lseek -Dmalloc=t_malloc -Dfree=t_free \
	    -Dprintf=t_printf -Dgetcwd=t_getcwd -Iinclude argscript/test/as_test.c argscript/argscript.c lib/args.c -o $@

# the nRF52 tty with EasyDMA, on registers the test plays (tests/nrf52/nrf.h)
build/test_nrf52: tests/test_nrf52.c tests/nrf52/nrf.h port/nrf52/nrf52.c port/nrf52/nrf52.h | build
	$(CC) $(CFLAGS) -Wno-pointer-to-int-cast $(SAN) -DNRF52_TTY_RX_PPI=3 -Itests/nrf52 tests/test_nrf52.c \
	    port/nrf52/nrf52.c -o $@

# DPX auto direction (opt/tty_dpx.h) on a cable model
build/test_tty_dpx: tests/test_tty_dpx.c opt/tty_dpx.h | build
	$(CC) $(CFLAGS) $(SAN) tests/test_tty_dpx.c -o $@

# the PIC24 tty on registers the test plays (tests/pic24/xc.h); a thread plays the interrupts
build/test_tty: tests/test_tty.c tests/pic24/xc.h port/pic24/tty.c opt/tty.h opt/tty_dpx.h | build
	$(CC) $(CFLAGS) $(SAN) -DTTY_PORTS=2 -Itests/pic24 tests/test_tty.c -lpthread -o $@

LIBS    = $(wildcard lib/*.c) $(wildcard opt/*.c)

libs: | build
	for f in $(LIBS); do $(CC) $(CFLAGS) $(PRELUDE) -Werror -c $$f -o build/$$(basename $$f .c).o || exit 1; done

test: build/test_kernel build/test_sleep build/test_wdt build/test_proc build/test_proc_fd build/test_vfs build/as_test build/test_nrf52 build/test_tty build/test_tty_dpx libs
	./build/test_kernel
	./build/test_sleep
	./build/test_wdt
	./build/test_proc
	./build/test_proc_fd
	./build/test_vfs
	./build/as_test
	./build/test_nrf52
	./build/test_tty
	./build/test_tty_dpx
	$(CC) $(CFLAGS) $(PRELUDE) -Werror -fsyntax-only -DTTY_PORTS=2 -DTTY_DPX_AUTO=0 -Itests/pic24 port/pic24/tty.c
	for f in $(KERNEL); do $(CC) $(CFLAGS) $(NOFS) $(PRELUDE) -Werror -fsyntax-only -DVFS_PROC=0 -DVFS_POLL=0 -DBIN_DATE=0 -DBIN_TOP=0 -DBIN_FILES=0 $$f || exit 1; done
	$(MAKE) -C lib/test

SOURCES = $(shell git ls-files '*.c' '*.h' 2>/dev/null)

format:
	clang-format -i $(SOURCES)

docs: | build
	doxygen Doxyfile

clean:
	rm -rf build
	for f in $(KERNEL); do $(CC) $(CFLAGS) $(NOFS) $(PRELUDE) -Werror -fsyntax-only -DVFS_PROC=0 -DVFS_POLL=0 $$f || exit 1; done
	$(MAKE) -C lib/test clean

.PHONY: all libs test format docs clean

# a program with standard headers only (tests/app/posix-app.c): XC16 patched by tools/xc16-install.sh,
# Cortex-M (newlib) with lets/include and the port's prelude
XC16_GCC ?= $(lastword $(sort $(wildcard /Applications/microchip/xc16/v*/bin/xc16-gcc /opt/microchip/xc16/v*/bin/xc16-gcc)))
app-check:
	@if [ -n "$(XC16_GCC)" ] && tools/xc16-install.sh --check >/dev/null; then \
		$(XC16_GCC) -mcpu=24FJ1024GA606 -omf=elf -O1 -Wall -Werror -c tests/app/posix-app.c -o build/posix-app-xc16.o && echo "app-check: XC16 ok"; \
	else echo "app-check: XC16 not found or not patched (tools/xc16-install.sh): skipped"; fi
	@if command -v arm-none-eabi-gcc >/dev/null; then \
		arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -Os -Wall -Werror -include port/cortex-m/prelude.h -Iinclude \
			-c tests/app/posix-app.c -o build/posix-app-arm.o && echo "app-check: Cortex-M ok"; \
	else echo "app-check: arm-none-eabi-gcc not found: skipped"; fi
.PHONY: app-check

