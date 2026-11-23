# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
BUILD ?= build

CORE_DIR := kernel/core
CORE := $(addprefix $(CORE_DIR)/,epd_wbf.c epd_tables.c epd_frame.c epd_engine.c epd_convert.c epd_policy.c \
	epd_ui.c epd_pattern.c epd_bootargs.c epd_ring.c)
NEON := $(CORE_DIR)/epd_neon.c
FUZZ_SRC := tests/fuzz_wbf.c tests/fixture.c tests/wbf_build.c
TEST_SRC := $(filter-out tests/fuzz_%.c,$(wildcard tests/*.c))
HEADERS := $(wildcard $(CORE_DIR)/*.h tests/*.h)
LINT_SRC := $(CORE) $(NEON) $(wildcard tests/*.c) tests/qemu/mkwbf.c

CFLAGS ?= -O1 -g
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wcast-align \
	-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wnested-externs \
	-Wundef -Wvla -Wformat=2 -Wpointer-arith -Wwrite-strings -Wimplicit-fallthrough -Wnull-dereference \
	-Wdouble-promotion -Werror
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
INC := -I$(CORE_DIR) -Itests
ALL_CFLAGS := -std=gnu11 $(CFLAGS) $(WARN) $(SAN) $(INC)

FUZZ_CC ?= clang
FUZZ_SECONDS ?= 60
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck

VARIANTS := scalar
ifneq ($(filter aarch64% arm64%,$(shell $(CC) -dumpmachine)),)
VARIANTS += neon
NEON_DEF := -DEPD_NEON
endif

.PHONY: help check test lint fuzz kernel-lint qemu-test clean $(addprefix test-,$(VARIANTS))

help:
	@echo "make check        test + lint"
	@echo "make test         unit tests of the e-ink core with ASan/UBSan ($(VARIANTS)); T=<substring> runs a subset"
	@echo "make lint         clang-tidy and cppcheck on the core and the tests"
	@echo "make fuzz         libFuzzer on the waveform parser and the engine for FUZZ_SECONDS=$(FUZZ_SECONDS)"
	@echo "make kernel-lint  KSRC=<kernel source> KOUT=<its build>: modules with W=1 -Werror, sparse, checkpatch"
	@echo "make qemu-test    KSRC=<kernel source> KOUT=<build with tests/qemu/kernel.config>: the modules in QEMU"
	@echo "make clean        remove $(BUILD)/"

check: test lint

test: $(addprefix test-,$(VARIANTS))

$(addprefix test-,$(VARIANTS)): test-%: $(BUILD)/test-%
	$< $(T)

$(BUILD)/test-scalar: $(CORE) $(TEST_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $(CORE) $(TEST_SRC)

$(BUILD)/test-neon: $(CORE) $(NEON) $(TEST_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(ALL_CFLAGS) -DEPD_NEON -o $@ $(CORE) $(NEON) $(TEST_SRC)

lint:
	$(CLANG_TIDY) --quiet $(LINT_SRC) -- -std=gnu11 $(NEON_DEF) $(INC)
	$(CPPCHECK) --enable=warning,performance,portability --inconclusive --std=c11 -q --check-level=exhaustive \
		--error-exitcode=1 --suppress=missingIncludeSystem --suppress='*OutOfMemory:tests/*' \
		--suppress=ctunullpointerOutOfMemory $(NEON_DEF) $(INC) $(CORE_DIR) tests

fuzz: $(BUILD)/fuzz-wbf
	@mkdir -p $(BUILD)/corpus
	$< -max_total_time=$(FUZZ_SECONDS) -max_len=4096 -print_final_stats=1 $(BUILD)/corpus

$(BUILD)/fuzz-wbf: $(CORE) $(FUZZ_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(FUZZ_CC) $(ALL_CFLAGS) -fsanitize=fuzzer -o $@ $(CORE) $(FUZZ_SRC)

kernel-lint:
	tests/kernel-lint.sh $(KSRC) $(KOUT)

qemu-test:
	tests/qemu/run.sh $(KSRC) $(KOUT)

clean:
	rm -rf $(BUILD)
