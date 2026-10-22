# PC build of the kernel (demo + tests). The MCU build uses your MPLAB X / XC16 project: add the
# kernel, fsi and lib sources to it (see docs/porting.md).
#
#   make          the host demos: build/lets-demo (a shell on your terminal) and
#                 build/lets-vfs-demo (the same with the virtual file system)
#   make test     every host test
#   make format   applies the code style (clang-format)
#   make docs     API reference (Doxygen) in build/docs/html

CC      ?= cc
WARN    = -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS  ?= -std=gnu11 -O1 -g $(WARN)
SAN     = -fsanitize=address,undefined
PRELUDE = -include port/host/prelude.h

KERNEL  = kernel/task.c kernel/alloc.c kernel/posix.c fsi/fsi.c lib/args.c \
          $(wildcard kernel/bin/*.c)

# the same with the virtual file system: vfs/posix.c instead of kernel/posix.c
VFS     = $(filter-out kernel/posix.c,$(KERNEL)) $(wildcard vfs/*.c vfs/dev/*.c)

all: build/lets-demo build/lets-vfs-demo

build:
	mkdir -p build

build/host_tty.o: port/host/host_tty.c port/host/host_tty.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/lets-demo: examples/host/main.c $(KERNEL) build/host_tty.o | build
	$(CC) $(CFLAGS) $(PRELUDE) examples/host/main.c $(KERNEL) build/host_tty.o -o $@

build/lets-vfs-demo: examples/host/vfs_main.c $(VFS) build/host_tty.o | build
	$(CC) $(CFLAGS) $(PRELUDE) examples/host/vfs_main.c $(VFS) build/host_tty.o -o $@

build/test_vfs: tests/test_vfs.c $(VFS) | build
	$(CC) $(CFLAGS) $(SAN) $(PRELUDE) tests/test_vfs.c $(VFS) -o $@

build/test_kernel: tests/test_kernel.c $(KERNEL) | build
	$(CC) $(CFLAGS) $(SAN) $(PRELUDE) tests/test_kernel.c $(KERNEL) -o $@

build/as_test: argscript/test/as_test.c argscript/argscript.c | build
	$(CC) -std=c99 $(WARN) $(SAN) -include fcntl.h -Dopen=t_open -Dread=t_read -Dwrite=t_write \
	    -Dclose=t_close -Dsystem=t_system argscript/test/as_test.c argscript/argscript.c -o $@

LIBS    = $(filter-out lib/uuid.c,$(wildcard lib/*.c))

libs: | build
	for f in $(LIBS); do $(CC) $(CFLAGS) $(PRELUDE) -Werror -c $$f -o build/$$(basename $$f .c).o || exit 1; done

test: build/test_kernel build/test_vfs build/as_test libs
	./build/test_kernel
	./build/test_vfs
	./build/as_test
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
