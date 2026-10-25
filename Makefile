CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS  += -Iinclude -MMD -MP

SRC  := $(wildcard src/*.c)
OBJ  := $(patsubst src/%.c,build/%.o,$(SRC))
TESTS := $(patsubst tests/test_%.c,build/test_%,$(wildcard tests/test_*.c))

.PHONY: all test clean

all: $(OBJ)

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# Each tests/test_X.c is linked against every library object (the library
# must therefore contain no main()).
build/test_%: tests/test_%.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -rf build

-include $(OBJ:.o=.d)