CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS  += -Iinclude -MMD -MP

SRC  := $(wildcard src/*.c)
OBJ  := $(patsubst src/%.c,build/%.o,$(SRC))
TESTS := $(patsubst tests/test_%.c,build/test_%,$(wildcard tests/test_*.c))
LIB  := build/libcpandas.a
EXAMPLES := build/demo build/basic

.PHONY: all test clean demo examples

# `make` builds the library and both example programs.
all: $(LIB) $(EXAMPLES)

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# The whole library as one static archive, so other programs can link it.
$(LIB): $(OBJ) | build
	ar rcs $@ $^

# Each tests/test_X.c is linked against every library object (the library
# must therefore contain no main()).
build/test_%: tests/test_%.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# Example programs: build/demo (interactive menu) and build/basic (a short
# program that uses the library step by step).
build/demo: examples/demo.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@

build/basic: examples/basic.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@

demo: build/demo

examples: $(EXAMPLES)

clean:
	rm -rf build

-include $(OBJ:.o=.d) $(TESTS:=.d) $(EXAMPLES:=.d)
