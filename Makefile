# C core for the CS370 term project. Python lives in sensors/ and tests/*.py.
CC      ?= cc
OPT     ?= -O2 -g
CFLAGS  := $(OPT) -std=c17 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -Isrc/common
BUILD   := build

COMMON  := src/common/crc32.c src/common/ipc.c
HEADERS := $(wildcard src/common/*.h)
BINS    := $(BUILD)/sensord $(BUILD)/hub $(BUILD)/store $(BUILD)/supervisor
TESTS   := $(BUILD)/test_crc32 $(BUILD)/test_record $(BUILD)/test_ipc

.PHONY: all test asan clean

all: $(BINS)

$(BUILD)/%: src/%/main.c $(COMMON) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BUILD)/test_%: tests/c/test_%.c $(COMMON) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BUILD):
	mkdir -p $@

test: $(TESTS)
	@for t in $(TESTS); do $$t || exit 1; done

# Same tests under AddressSanitizer + UBSan, in a separate build dir.
asan:
	$(MAKE) clean
	$(MAKE) test BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

clean:
	rm -rf build build-asan
