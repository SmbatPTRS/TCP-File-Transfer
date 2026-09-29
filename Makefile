.RECIPEPREFIX = >
CC     = gcc
CFLAGS = -Wall -Wextra -g

all: build/sender build/receiver

build/%: src/%.c | build
> $(CC) $(CFLAGS) $< -o $@

build:
> mkdir -p build

clean:
> rm -rf build

.PHONY: all clean
