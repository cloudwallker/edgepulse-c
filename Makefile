CC ?= gcc
PYTHON ?= python3
BUILD_DIR ?= build
CFLAGS ?= -O2 -g
STRICT = -std=c11 -Wall -Wextra -Werror -Wpedantic -Iinclude
CORE = src/protocol.c src/store.c src/device.c src/receiver.c
FILE_BACKEND = src/file_store.c
HEADERS = include/ep_protocol.h include/ep_store.h include/ep_device.h include/ep_file_store.h include/ep_receiver.h
TESTS = $(BUILD_DIR)/test_protocol $(BUILD_DIR)/test_store $(BUILD_DIR)/test_device $(BUILD_DIR)/test_reliable $(BUILD_DIR)/test_receiver
WINDOWS_ENTRY =
TARGET_MACHINE := $(shell $(CC) -dumpmachine)
ifneq (,$(findstring mingw,$(TARGET_MACHINE)))
WINDOWS_ENTRY = -municode
endif

.PHONY: all test demo sanitize
all: $(BUILD_DIR)/edgepulse

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/edgepulse: src/main.c $(CORE) $(FILE_BACKEND) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STRICT) $(CFLAGS) $(WINDOWS_ENTRY) $(CORE) $(FILE_BACKEND) src/main.c -o "$@" $(LDFLAGS)

$(BUILD_DIR)/test_%: tests/test_%.c $(CORE) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STRICT) $(CFLAGS) $(CORE) "$<" -o "$@" $(LDFLAGS)

$(BUILD_DIR)/file_store_worker: tests/file_store_worker.c $(CORE) $(FILE_BACKEND) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STRICT) $(CFLAGS) $(WINDOWS_ENTRY) -DEP_FILE_TESTING $(CORE) $(FILE_BACKEND) tests/file_store_worker.c -o "$@" $(LDFLAGS)

test: $(BUILD_DIR)/edgepulse $(TESTS) $(BUILD_DIR)/file_store_worker
	$(BUILD_DIR)/test_protocol
	$(BUILD_DIR)/test_store
	$(BUILD_DIR)/test_device
	$(BUILD_DIR)/test_reliable
	$(BUILD_DIR)/test_receiver
	$(PYTHON) tests/test_cli.py $(BUILD_DIR)/edgepulse
	$(PYTHON) tests/test_file_store.py $(BUILD_DIR)/file_store_worker

demo: $(BUILD_DIR)/edgepulse
	$(BUILD_DIR)/edgepulse --demo

sanitize:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) BUILD_DIR=build/sanitize CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" test
