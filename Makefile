# moldwatch
#   make            build everything into build/
#   make test       build and run the unit tests
#   make asan       same, under AddressSanitizer + UBSan (into build/asan)
#   make memcheck   run tests and a fake capture under valgrind
#   make clean

CC       ?= gcc
BUILD    ?= build
EXTRA    ?=
CFLAGS   := -std=c17 -Wall -Wextra -Werror -O2 -g $(EXTRA)
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L

CAPTURE_SRC := src/capture/capture.c src/capture/i2c.c src/capture/bme280.c \
               src/capture/mlx90614.c src/capture/fake.c src/capture/log.c
CAPTURE_HDR := include/record.h $(wildcard src/capture/*.h)

.PHONY: all test asan memcheck soakcheck clean

all: $(BUILD)/capture

$(BUILD)/capture: $(CAPTURE_SRC) $(CAPTURE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc/capture $(CFLAGS) -o $@ $(CAPTURE_SRC)

$(BUILD)/test_record: tests/test_record.c src/capture/fake.c $(CAPTURE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc/capture $(CFLAGS) -o $@ tests/test_record.c src/capture/fake.c

test: $(BUILD)/test_record
	./$(BUILD)/test_record

asan:
	$(MAKE) BUILD=build/asan EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer" all test

memcheck: $(BUILD)/capture $(BUILD)/test_record
	valgrind --error-exitcode=1 --leak-check=full -q ./$(BUILD)/test_record
	valgrind --error-exitcode=1 --leak-check=full -q ./$(BUILD)/capture --sensor bme280 --fake \
		--interval-ms 10 --count 20 --fake-drop 5:3 > /dev/null

soakcheck:
	@echo "soakcheck needs storage and the supervisor; not built yet"

clean:
	rm -rf build
