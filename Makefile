# moldwatch -- build skeleton.
# TODO(M2/M3): real targets as src/ fills in. Kept minimal on purpose so
# the first real Makefile work happens in our own Claude Code sessions.

CC      := gcc
CFLAGS  := -std=c17 -Wall -Wextra -Werror -O2
ASAN    := -fsanitize=address,undefined -g -O1
BUILD   := build

.PHONY: all test asan memcheck soakcheck clean

all:
	@mkdir -p $(BUILD)
	@echo "nothing to build yet (M0/M1)"

test:
	@echo "no tests yet (M0/M1)"

asan:
	@echo "no asan build yet (M0/M1)"

memcheck:
	@echo "no memcheck target yet (M0/M1)"

soakcheck:
	@echo "no soak target yet (M0/M1)"

clean:
	rm -rf $(BUILD)
