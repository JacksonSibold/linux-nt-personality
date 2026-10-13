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
# exports here. TEMP_DIR is taken from the environment if set, else TEMP, TMP,
# LOCALAPPDATA/Temp, and finally MSYS /tmp (which maps to the Windows temp
# directory). Override it on the command line if needed.
TEMP_DIR ?= $(if $(TEMP),$(TEMP),$(if $(TMP),$(TMP),$(if $(LOCALAPPDATA),$(shell cygpath -m "$(LOCALAPPDATA)/Temp"),$(shell cygpath -m /tmp))))
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
# COVERAGE=1: gcov instrumentation at -O0 in out-cov/ (see tools/c_coverage.sh).
COVERAGE ?= 0
ifeq ($(COVERAGE),1)
OUT      := out-cov
COV_FLAGS := --coverage
COV_LIBS  := -lgcov
OPT_LEVEL := -O0
else
OUT      := out
COV_FLAGS :=
COV_LIBS  :=
OPT_LEVEL := -O1
endif
TEST     := $(OUT)/test_pe_parse$(EXE_SUFFIX)

# Linker-plugin workaround for the MSYS2 gcc (liblto_plugin.dll fails to load).
# Harmless on other gcc builds.
LDFLAGS  := -fno-use-linker-plugin $(COV_FLAGS)

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
CFLAGS   := -std=c11 -g $(OPT_LEVEL) -Wall -Wextra -Werror $(COV_FLAGS) $(SAN_FLAGS)

OBJS := $(OUT)/pe_parse.o $(OUT)/pe_image.o $(OUT)/test_pe_parse.o

.PHONY: all test real-pe clean

all: test

# Real PE images built at test time by the MinGW gcc (see fixtures/). The
# images are only read as byte buffers by test_real_pe.c and are never run.
# Each fixture gets one rule below and is listed in FIX_EXES.
FIX_SRC     := fixtures
FIX_OUT     := $(OUT)/fixtures
FIX_EXES    := $(FIX_OUT)/minimal.exe $(FIX_OUT)/exports.dll
FIX_CFLAGS  := -std=c11 -O1 -fno-pic -fno-stack-protector -fno-ident -Wall -Wextra -Werror
FIX_LDFLAGS := -fno-use-linker-plugin -nostdlib

REAL_TEST := $(OUT)/test_real_pe$(EXE_SUFFIX)
REAL_OBJS := $(OUT)/pe_parse.o $(OUT)/test_real_pe.o

test: $(TEST) $(REAL_TEST) $(FIX_EXES)
	./$(TEST) 2>$(OUT)/pe_diag.log
	@echo "host-test: pe_validate diagnostics in $(OUT)/pe_diag.log"
	./$(REAL_TEST) 2>>$(OUT)/pe_diag.log

real-pe: $(REAL_TEST) $(FIX_EXES)
	./$(REAL_TEST) 2>>$(OUT)/pe_diag.log

$(TEST): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@ $(COV_LIBS)

$(REAL_TEST): $(REAL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(REAL_OBJS) -o $@

$(FIX_OUT)/minimal.exe: $(FIX_SRC)/minimal.c | $(FIX_OUT)
	$(CC) $(FIX_CFLAGS) $(FIX_LDFLAGS) -Wl,-e,minimal_entry $< -o $@

$(FIX_OUT)/exports.dll: $(FIX_SRC)/exports.c $(FIX_SRC)/exports.def | $(FIX_OUT)
	$(CC) $(FIX_CFLAGS) $(FIX_LDFLAGS) -shared -Wl,-e,exports_entry $(FIX_SRC)/exports.c $(FIX_SRC)/exports.def -o $@

$(FIX_OUT):
	mkdir -p $(FIX_OUT)

$(OUT)/pe_parse.o: $(SRC)/pe_parse.c $(SRC)/pe_parse.h $(SRC)/pe_format.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/%.o: %.c minitest.h pe_image.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)
