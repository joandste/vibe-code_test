CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic

.PHONY: all test clean

all: lisp

lisp: main.c lisp.c lisp.h
	$(CC) $(CFLAGS) main.c lisp.c -lm -o $@

test: test.c lisp.c lisp.h
	$(CC) $(CFLAGS) test.c lisp.c -lm -o test
	./test

clean:
	rm -f lisp test
