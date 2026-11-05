# Host unit tests:  make -C lib/test
CC ?= cc
CFLAGS ?= -std=c99 -O1 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined
TESTS = test_byte8

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_byte8: test_byte8.c ../byte8.c ../byte8.h
	$(CC) $(CFLAGS) -Dread=test_read -Dwrite=test_write -o $@ test_byte8.c ../byte8.c

clean:
	rm -rf $(TESTS) *.dSYM

.PHONY: all clean
