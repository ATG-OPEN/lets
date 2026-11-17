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
PRELUDE = -include port/host/prelude.h

# the kernel, its shell and vfs without tfs (VFS_NO_TFS, as on a small MCU) ...
KERNEL  = kernel/task.c kernel/alloc.c lib/args.c $(wildcard bin/*.c) \
          vfs/node.c vfs/vfs.c vfs/posix.c vfs/bin.c vfs/onefs.c
NOFS    = -DVFS_NO_TFS

# ... and with tfs and the memory drivers
VFS     = $(KERNEL) vfs/tfs.c vfs/tfsvfs.c vfs/ed.c $(filter-out vfs/dev/flash24.c,$(wildcard vfs/dev/*.c))

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

build/as_test: argscript/test/as_test.c argscript/argscript.c lib/args.c | build
	$(CC) -std=c99 $(WARN) $(SAN) -include fcntl.h -Dopen=t_open -Dread=t_read -Dwrite=t_write \
	    -Dclose=t_close -Dsystem=t_system -Dlseek=t_lseek -Dmalloc=t_malloc -Dfree=t_free \
	    -Dprintf=t_printf argscript/test/as_test.c argscript/argscript.c lib/args.c -o $@

# the nRF52 tty with EasyDMA, on registers the test plays (tests/nrf52/nrf.h)
build/test_nrf52: tests/test_nrf52.c tests/nrf52/nrf.h port/nrf52/nrf52.c port/nrf52/nrf52.h | build
	$(CC) $(CFLAGS) -Wno-pointer-to-int-cast $(SAN) -DNRF52_TTY_RX_PPI=3 -Itests/nrf52 tests/test_nrf52.c \
	    port/nrf52/nrf52.c -o $@

# the PIC24 tty on registers the test plays (tests/pic24/xc.h); a thread plays the interrupts
build/test_tty: tests/test_tty.c tests/pic24/xc.h opt/tty.c opt/tty.h | build
	$(CC) $(CFLAGS) $(SAN) -DTTY_PORTS=2 -Itests/pic24 tests/test_tty.c -lpthread -o $@

LIBS    = $(filter-out lib/uuid.c opt/tty.c,$(wildcard lib/*.c) $(wildcard opt/*.c))

libs: | build
	for f in $(LIBS); do $(CC) $(CFLAGS) $(PRELUDE) -Werror -c $$f -o build/$$(basename $$f .c).o || exit 1; done

test: build/test_kernel build/test_sleep build/test_vfs build/as_test build/test_nrf52 build/test_tty libs
	./build/test_kernel
	./build/test_sleep
	./build/test_vfs
	./build/as_test
	./build/test_nrf52
	./build/test_tty
	$(MAKE) -C lib/test

SOURCES = $(shell git ls-files '*.c' '*.h' 2>/dev/null)

format:
	clang-format -i $(SOURCES)

docs: | build
	doxygen Doxyfile

clean:
	rm -rf build
	$(MAKE) -C lib/test clean

.PHONY: all libs test format docs clean
