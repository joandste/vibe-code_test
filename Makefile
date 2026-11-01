CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude -Isrc
LDLIBS ?= -lm

LIB_SRC := src/value.c src/env.c src/reader.c src/eval.c src/prims.c src/api.c
LIB_OBJ := $(LIB_SRC:.c=.o)
BIN_OBJ := src/main.o

all: langrun

langrun: $(LIB_OBJ) $(BIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

lib: liblang.a

liblang.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

tests/embed_test: tests/embed_test.c liblang.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< liblang.a $(LDLIBS)

embed: examples/embed.c liblang.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< liblang.a $(LDLIBS)

example: embed

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

test: langrun tests/embed_test
	./tests/run.sh
	./tests/embed_test

clean:
	rm -f $(LIB_OBJ) $(BIN_OBJ) langrun liblang.a embed tests/embed_test
	rm -f tests/*.actual tests/cases/*.actual tests/errors/*.actual

.PHONY: all lib test clean example
