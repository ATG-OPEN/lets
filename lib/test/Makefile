# Host unit tests for the portable libraries:  make -C lib/test
CC ?= cc
CFLAGS ?= -std=c99 -O1 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined
TESTS = test_iir test_pulse test_apdbias test_spiral

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_iir: test_iir.c ../iir.c ../iir.h
	$(CC) $(CFLAGS) -o $@ test_iir.c ../iir.c -lm
test_pulse: test_pulse.c ../pulse.c ../pulse.h
	$(CC) $(CFLAGS) -o $@ test_pulse.c ../pulse.c -lm
test_apdbias: test_apdbias.c ../apdbias.c ../apdbias.h
	$(CC) $(CFLAGS) -o $@ test_apdbias.c ../apdbias.c -lm
test_spiral: test_spiral.c ../spiral.c ../spiral.h
	$(CC) $(CFLAGS) -o $@ test_spiral.c ../spiral.c -lm

clean:
	rm -f $(TESTS)

.PHONY: all clean
