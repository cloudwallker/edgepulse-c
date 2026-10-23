CC ?= gcc
PYTHON ?= python3
BUILD_DIR ?= build
CFLAGS ?= -O2 -g
STRICT = -std=c11 -Wall -Wextra -Werror -Wpedantic -Iinclude
CORE = src/protocol.c src/store.c src/device.c
HEADERS = include/ep_protocol.h include/ep_store.h include/ep_device.h
TESTS = $(BUILD_DIR)/test_protocol $(BUILD_DIR)/test_store $(BUILD_DIR)/test_device

.PHONY: all test demo sanitize
all: $(BUILD_DIR)/edgepulse

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/edgepulse: src/main.c $(CORE) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STRICT) $(CFLAGS) $(CORE) src/main.c -o "$@" $(LDFLAGS)

$(BUILD_DIR)/test_%: tests/test_%.c $(CORE) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STRICT) $(CFLAGS) $(CORE) "$<" -o "$@" $(LDFLAGS)

test: $(BUILD_DIR)/edgepulse $(TESTS)
	$(BUILD_DIR)/test_protocol
	$(BUILD_DIR)/test_store
	$(BUILD_DIR)/test_device
	$(PYTHON) tests/test_cli.py $(BUILD_DIR)/edgepulse

demo: $(BUILD_DIR)/edgepulse
	$(BUILD_DIR)/edgepulse --demo

sanitize:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) BUILD_DIR=build/sanitize CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" test
