# Host tests for the C runtime in src/crt. Each test compiles the runtime headers natively and compares the
# functions with the system C library (differential tests) or with values worked out by hand.
#
#   make -C tests/crt-host test        build and run every test
#   make -C tests/crt-host clean
#
# The runtime is freestanding and prefixes its functions ucrt_, so it can sit beside glibc in one binary.
CC       ?= gcc
SRC      := ../../src/crt
NTHOST   := ../ntdll-host
OUT      := out
SAN_FLAGS ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS   := -std=c11 -g -O1 -Wall -Wextra -Werror -D_GNU_SOURCE $(SAN_FLAGS) -I$(SRC) -I$(NTHOST)
LDLIBS   := -lm

TESTS := test_string test_ctype

.PHONY: all test clean
all: test

test: $(addprefix $(OUT)/,$(TESTS))
	@for t in $(addprefix $(OUT)/,$(TESTS)); do ./$$t || exit 1; done
	@echo "crt-host: all tests passed"

$(OUT):
	mkdir -p $(OUT)

$(OUT)/%: %.c $(wildcard $(SRC)/*.h) $(NTHOST)/minitest.h | $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -rf $(OUT)
