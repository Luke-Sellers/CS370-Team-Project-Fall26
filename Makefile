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

# --- storage (src/store); see docs/store-plan.md ---
STORE_HDR := include/record.h $(wildcard src/store/*.h)

.PHONY: test-store-crc

$(BUILD)/test_store_crc: tests/test_store_crc.c src/store/crc32.c $(STORE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc/store $(CFLAGS) -o $@ tests/test_store_crc.c src/store/crc32.c

test: test-store-crc
test-store-crc: $(BUILD)/test_store_crc
	./$(BUILD)/test_store_crc

# store.c logs through capture's log.c, compiled in unmodified.
STORE_LIB := src/store/store.c src/store/crc32.c src/capture/log.c

.PHONY: test-store-header

$(BUILD)/test_store_header: tests/test_store_header.c $(STORE_LIB) $(STORE_HDR) src/capture/log.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc/store -Isrc/capture $(CFLAGS) -o $@ tests/test_store_header.c $(STORE_LIB)

test: test-store-header
test-store-header: $(BUILD)/test_store_header
	./$(BUILD)/test_store_header
