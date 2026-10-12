# Host-side tests for src/loader (pe_parse.c). Plain gcc, no kernel headers:
# pe_parse.c only pulls in stdio/string/stdarg when __KERNEL__ is not defined.
#
#   make -C tests/host test          build and run
#   make -C tests/host SANITIZE=0    never try -fsanitize
#
# SANITIZE=auto (default) enables -fsanitize=address,undefined only when a
# probe program both links and runs with it. The MSYS2 UCRT64 gcc on the
# development machine ships no libasan/libubsan, so there it builds without
# sanitizers.

# Under MSYS2 make does not see the OS variable, so also check uname.
UNAME_S := $(shell uname -s 2>/dev/null)
ifneq (,$(filter Windows_NT,$(OS))$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
# Same exports as the toolchain notes (TOOLCHAIN.md, kept outside the repo).
# Without TMP the link step fails with "Cannot create temporary file in
# C:\WINDOWS\". The recipe shell does not reliably inherit them, hence the
# exports here. TEMP_DIR defaults to the per-user temp directory; override it
# in the environment if needed.
TEMP_DIR ?= $(if $(LOCALAPPDATA),$(shell cygpath -m "$(LOCALAPPDATA)/Temp"),C:/Temp)
export TMP    := $(TEMP_DIR)
export TEMP   := $(TEMP_DIR)
export TMPDIR := $(shell cygpath -u "$(TEMP_DIR)")
EXE_SUFFIX := .exe
else
EXE_SUFFIX :=
endif

# make's built-in CC default is "cc", so ?= would not take effect. Override on
# the command line with make CC=... if needed.
CC       := gcc
SANITIZE ?= auto
SRC      := ../../src/loader
OUT      := out
TEST     := $(OUT)/test_pe_parse$(EXE_SUFFIX)

# Linker-plugin workaround for the MSYS2 gcc (liblto_plugin.dll fails to load).
# Harmless on other gcc builds.
LDFLAGS  := -fno-use-linker-plugin

ifneq ($(SANITIZE),0)
SAN_OK := $(shell mkdir -p $(OUT) && printf 'int main(void){return 0;}\n' > $(OUT)/san_probe.c && $(CC) -fsanitize=address,undefined $(LDFLAGS) $(OUT)/san_probe.c -o $(OUT)/san_probe$(EXE_SUFFIX) >/dev/null 2>&1 && $(OUT)/san_probe$(EXE_SUFFIX) >/dev/null 2>&1 && echo yes)
endif
ifeq ($(SAN_OK),yes)
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
$(info host-test: AddressSanitizer and UBSan enabled)
else
SAN_FLAGS :=
$(info host-test: sanitizers unavailable, building without them)
endif

CPPFLAGS := -I$(SRC)
CFLAGS   := -std=c11 -g -O1 -Wall -Wextra -Werror $(SAN_FLAGS)

OBJS := $(OUT)/pe_parse.o $(OUT)/pe_image.o $(OUT)/test_pe_parse.o

.PHONY: all test clean

all: test

test: $(TEST)
	./$(TEST) 2>$(OUT)/pe_diag.log
	@echo "host-test: pe_validate diagnostics in $(OUT)/pe_diag.log"

$(TEST): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(OUT)/pe_parse.o: $(SRC)/pe_parse.c $(SRC)/pe_parse.h $(SRC)/pe_format.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/%.o: %.c minitest.h pe_image.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)
