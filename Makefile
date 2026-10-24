CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined

test: tests/test_series.c
	$(CC) $(CFLAGS) tests/test_series.c -o test
	./test

clean:
	rm -f test