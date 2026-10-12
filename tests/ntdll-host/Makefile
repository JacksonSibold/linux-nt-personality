# Host-side tests for pure ntdll code (src/ntdll). Plain gcc, no NT headers
# beyond nt_types.h and ntstatus.h.
#
#   make -C tests/ntdll-host test          build and run
#   make -C tests/ntdll-host SANITIZE=0    never try -fsanitize
#
# Coverage:
#   test_ntstatus_to_dos  RtlNtStatusToDosError table invariants and golden values
#   test_unwind           RUNTIME_FUNCTION lookup for registered modules
#   test_except           vectored exception handler return codes
#   test_runtime          freestanding malloc/calloc from ntdll_runtime.c
#   test_win32_policy     kernel32 shim argument mapping (win32_policy.h)
#   test_pe_bounds        PE section and relocation range checks (pe_bounds.h)
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
SRC      := ../../src/ntdll
LOADER   := ../../src/loader
OUT      := out

# Linker-plugin workaround for the MSYS2 gcc (liblto_plugin.dll fails to load).
# Harmless on other gcc builds.
LDFLAGS  := -fno-use-linker-plugin

ifneq ($(SANITIZE),0)
SAN_OK := $(shell mkdir -p $(OUT) && printf 'int main(void){return 0;}\n' > $(OUT)/san_probe.c && $(CC) -fsanitize=address,undefined $(LDFLAGS) $(OUT)/san_probe.c -o $(OUT)/san_probe$(EXE_SUFFIX) >/dev/null 2>&1 && $(OUT)/san_probe$(EXE_SUFFIX) >/dev/null 2>&1 && echo yes)
endif
ifeq ($(SAN_OK),yes)
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
$(info ntdll-host: AddressSanitizer and UBSan enabled)
else
SAN_FLAGS :=
$(info ntdll-host: sanitizers unavailable, building without them)
endif

CPPFLAGS := -I$(SRC) -I$(LOADER)
CFLAGS   := -std=c11 -g -O1 -Wall -Wextra -Werror $(SAN_FLAGS)

TESTS    := test_ntstatus_to_dos test_unwind test_except test_runtime test_win32_policy test_pe_bounds
TEST_BINS := $(addprefix $(OUT)/,$(addsuffix $(EXE_SUFFIX),$(TESTS)))

# test_pe_bounds is build-only on this host: Windows Application Control blocks
# the freshly built exe at run time (docs/review/ntdll.md, MAIN-1). It is still
# compiled by `make all` and run by `make test` on hosts that allow it, via
# `make test-pe-bounds`. Do not rename or relocate the binary to dodge the policy.
RUN_TESTS := $(filter-out test_pe_bounds,$(TESTS))
RUN_BINS  := $(addprefix $(OUT)/,$(addsuffix $(EXE_SUFFIX),$(RUN_TESTS)))

.PHONY: all test test-pe-bounds build clean

all: build test

build: $(TEST_BINS)

test: $(RUN_BINS)
	@for t in $(RUN_BINS); do ./$$t || exit 1; done
	@echo "ntdll-host: all tests passed (test_pe_bounds build-only on this host)"

test-pe-bounds: $(OUT)/test_pe_bounds$(EXE_SUFFIX)
	./$(OUT)/test_pe_bounds$(EXE_SUFFIX)

# Each test binary: the test file plus the source objects it exercises.
# The test #includes ntstatus_to_dos.c, so that file and the headers it uses are prerequisites.
$(OUT)/test_ntstatus_to_dos$(EXE_SUFFIX): test_ntstatus_to_dos.c minitest.h $(SRC)/ntstatus_to_dos.c $(SRC)/ntstatus.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only mapping tests (src/ntdll/win32_policy.h).
$(OUT)/test_win32_policy$(EXE_SUFFIX): test_win32_policy.c minitest.h $(SRC)/win32_policy.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only range checks (src/ntdll/pe_bounds.h).
$(OUT)/test_pe_bounds$(EXE_SUFFIX): test_pe_bounds.c minitest.h $(SRC)/pe_bounds.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# unwind.c calls _NtRaiseException from except.c (RtlRaiseException).
# except.c also defines SetUnhandledExceptionFilter and UnhandledExceptionFilter,
# which the MinGW libkernel32 import library exports too. The objects come
# before the libraries on the link line, so the first (ntdll) definition wins;
# --allow-multiple-definition only silences the duplicate from the library.
$(OUT)/test_unwind$(EXE_SUFFIX): $(OUT)/unwind.o $(OUT)/except.o $(OUT)/test_unwind.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--allow-multiple-definition $^ -o $@

$(OUT)/test_except$(EXE_SUFFIX): $(OUT)/except.o $(OUT)/unwind.o $(OUT)/test_except.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--allow-multiple-definition $^ -o $@

# ntdll_runtime.c replaces libc string and allocator functions. Keep gcc from
# turning its loops back into calls to memcpy/memset.
$(OUT)/test_runtime$(EXE_SUFFIX): $(OUT)/ntdll_runtime.o $(OUT)/test_runtime.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

# -Wno-error=attributes: ntdll_runtime.c declares "int errno" and "_errno", which
# clash with the dllimport attribute on MinGW's _errno (see docs/review/ntdll.md).
$(OUT)/ntdll_runtime.o: EXTRA_CFLAGS := -fno-builtin -fno-tree-loop-distribute-patterns -Wno-error=attributes

$(OUT)/%.o: $(SRC)/%.c | $(OUT)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/test_%.o: test_%.c minitest.h | $(OUT)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(CPPFLAGS) -c $< -o $@

# The runtime test calls calloc and malloc by name. Without -fno-builtin gcc
# treats them as built-ins and the overflow checks are not exercised.
$(OUT)/test_runtime.o: EXTRA_CFLAGS := -fno-builtin

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)
