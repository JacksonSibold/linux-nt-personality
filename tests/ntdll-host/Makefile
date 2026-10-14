# Host-side tests for pure ntdll code (src/ntdll). Plain gcc, no NT headers
# beyond nt_types.h and ntstatus.h.
#
#   make -C tests/ntdll-host test          build and run
#   make -C tests/ntdll-host SANITIZE=0    never try -fsanitize
#
# Coverage:
#   test_ntstatus_to_dos  RtlNtStatusToDosError table invariants and golden values
#   test_unwind           RUNTIME_FUNCTION lookup for registered modules
#   test_unwind_pure      UWOP_* decoding, prolog and chained info (unwind_pure.h)
#   test_except           vectored exception handler return codes
#   test_except_pure      list order, resume decisions, vectored continue handlers (except_pure.h)
#   test_runtime          freestanding malloc/calloc from ntdll_runtime.c
#   test_win32_policy     kernel32 shim argument mapping (win32_policy.h)
#   test_msvcrt_policy    msvcrt calloc size overflow check, snprintf return value (msvcrt_policy.h, msvcrt_format.h)
#   test_pe_bounds        PE section and relocation range checks (pe_bounds.h)
#   test_ldr_pure         loader IAT slot writes and TLS callback table reads (ldr_pure.h)
#   test_ldr_mapped_pure  startup-module headers, RVA-indexed exports and forwarders (ldr_pure.h)
#   test_advapi_pure      advapi32 registry key, value and path mapping (advapi_pure.h)
#   test_ldr_sched        loader init order, refcounts, search order, names, attach result (ldr_sched.h)
#   test_k32_module_pure  Win32 module API helpers: conversion, flags, names, errors (k32_module_pure.h)
#   test_k32_io_pure      std handles, ReadFile/WriteFile status, console mode and text (k32_io_pure.h)
#   test_k32_sync_pure    WaitForMultipleObjects args and results, critical-section counts, GetTickCount (k32_sync_pure.h)
#   test_k32_mm_pure      CreateFileMapping flags, MapViewOfFile access and offsets (k32_mm_pure.h)
#   test_k32_thread_pure  CreateThread flags, exit codes, suspend results (k32_thread_pure.h)
#   test_k32_find_pure    FindFirstFile wildcards, temp path and temp names, environment match (k32_find_pure.h)
#   test_k32_sys_pure     thread and process priority, handle flags, PE binary type (k32_sys_pure.h)
#   test_k32_str_pure     UTF-8 and Windows-1252 conversions, buffer sizes, default character (k32_str_pure.h)
#   test_k32_env_pure     environment block length, lookup, set/delete build, %NAME% expansion (k32_env_pure.h)
#   test_w32k_window      win32k model: classes, window lifecycle, properties, create/destroy message order
#   test_w32k_msg         win32k model: queues, post/send/get/peek order, timers, focus, capture, hooks
#   test_w32k_input       win32k model: keyboard and mouse routing, async state, evdev decoding
#   test_w32k_sys         win32k model: clipboard, metrics, monitors, display settings, stations, cursors
#   test_w32k_gdi         win32k model: DC, surfaces, blits, stock and selectable objects, arena
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
# COVERAGE=1: gcov instrumentation at -O0 in out-cov/ (see tools/c_coverage.sh).
# --coverage at link time also pulls in libgcov.
COVERAGE ?= 0
ifeq ($(COVERAGE),1)
OUT      := out-cov
COV_FLAGS := --coverage
OPT_LEVEL := -O0
else
OUT      := out
COV_FLAGS :=
OPT_LEVEL := -O1
endif

# Linker-plugin workaround for the MSYS2 gcc (liblto_plugin.dll fails to load).
# Harmless on other gcc builds.
LDFLAGS  := -fno-use-linker-plugin $(COV_FLAGS)

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

W32K     := ../../src/ntdll/win32k
CPPFLAGS := -I$(SRC) -I$(LOADER) -I$(W32K)
# _GNU_SOURCE: except.c and the headers it shares with the tests need siginfo_t,
# sigaction and ucontext REG_* on glibc. Under -std=c11 they are hidden unless it is
# defined before the first system header, which minitest.h's stdio.h already is.
CFLAGS   := -std=c11 -g $(OPT_LEVEL) -Wall -Wextra -Werror -D_GNU_SOURCE $(COV_FLAGS) $(SAN_FLAGS)

TESTS    := test_ntstatus_to_dos test_advapi_pure test_unwind test_unwind_pure test_except test_except_pure test_runtime test_win32_policy test_msvcrt_policy test_pe_bounds test_ldr_pure test_ldr_mapped_pure test_ldr_sched test_k32_module_pure test_k32_io_pure test_k32_file_pure test_k32_sync_pure test_k32_mm_pure test_k32_thread_pure test_k32_find_pure test_k32_sys_pure test_k32_str_pure test_k32_env_pure test_w32k_window test_w32k_msg test_w32k_input test_w32k_sys test_w32k_gdi
TEST_BINS := $(addprefix $(OUT)/,$(addsuffix $(EXE_SUFFIX),$(TESTS)))

# test_pe_bounds is build-only on this host: Windows Application Control blocks
# the freshly built exe at run time (docs/review/ntdll.md, MAIN-1). It is still
# compiled by `make all` and run by `make test` on hosts that allow it, via
# `make test-pe-bounds`. Do not rename or relocate the binary to dodge the policy.
RUN_TESTS := $(filter-out test_pe_bounds,$(TESTS))
RUN_BINS  := $(addprefix $(OUT)/,$(addsuffix $(EXE_SUFFIX),$(RUN_TESTS)))

.PHONY: all test test-pe-bounds build clean print-run-bins w32k-abi

all: build test

build: $(TEST_BINS)

test: $(RUN_BINS)
	@for t in $(RUN_BINS); do ./$$t || exit 1; done
	@echo "ntdll-host: all tests passed (test_pe_bounds build-only on this host)"

# Compile-time ABI check of the win32k shims against the MinGW headers (w32k_abi.h). Build-only:
# the checked file is never run (Windows Application Control, MAIN-1).
MINGW_CC ?= x86_64-w64-mingw32-gcc
w32k-abi:
	$(MINGW_CC) -std=c11 -fsyntax-only -Wall -Wextra -I$(SRC) -I$(W32K) check_w32k_abi.c
	@echo "w32k-abi: layouts match the MinGW headers"

test-pe-bounds: $(OUT)/test_pe_bounds$(EXE_SUFFIX)
	./$(OUT)/test_pe_bounds$(EXE_SUFFIX)

# Each test binary: the test file plus the source objects it exercises.
# The test #includes ntstatus_to_dos.c, so that file and the headers it uses are prerequisites.
$(OUT)/test_ntstatus_to_dos$(EXE_SUFFIX): test_ntstatus_to_dos.c minitest.h $(SRC)/ntstatus_to_dos.c $(SRC)/ntstatus.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only mapping tests (src/ntdll/win32_policy.h).
$(OUT)/test_win32_policy$(EXE_SUFFIX): test_win32_policy.c minitest.h $(SRC)/win32_policy.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only size arithmetic for msvcrt calloc and the snprintf formatter
# (src/ntdll/msvcrt_policy.h, src/ntdll/msvcrt_format.h).
$(OUT)/test_msvcrt_policy$(EXE_SUFFIX): test_msvcrt_policy.c minitest.h $(SRC)/msvcrt_policy.h $(SRC)/msvcrt_format.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only range checks (src/ntdll/pe_bounds.h).
$(OUT)/test_pe_bounds$(EXE_SUFFIX): test_pe_bounds.c minitest.h $(SRC)/pe_bounds.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only loader helpers (src/ntdll/ldr_sched.h, used by ldr.c).
$(OUT)/test_ldr_sched$(EXE_SUFFIX): test_ldr_sched.c minitest.h $(SRC)/ldr_sched.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only Win32 module helpers (src/ntdll/k32_module_pure.h, used by kernel32.c).
# Includes ntstatus_to_dos.c for the RtlNtStatusToDosError cross-check.
$(OUT)/test_k32_module_pure$(EXE_SUFFIX): test_k32_module_pure.c minitest.h $(SRC)/k32_module_pure.h $(SRC)/ntstatus_to_dos.c $(SRC)/ntstatus.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only file helpers: access, disposition, path conversion (src/ntdll/k32_file_pure.h, used by kernel32.c).
$(OUT)/test_k32_file_pure$(EXE_SUFFIX): test_k32_file_pure.c minitest.h $(SRC)/k32_file_pure.h $(SRC)/nt_types.h $(SRC)/ntstatus.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only handle, console and I/O status helpers (src/ntdll/k32_io_pure.h, used by kernel32.c).
$(OUT)/test_k32_io_pure$(EXE_SUFFIX): test_k32_io_pure.c minitest.h $(SRC)/k32_io_pure.h $(SRC)/k32_module_pure.h $(SRC)/nt_types.h $(SRC)/ntstatus.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only critical-section, multi-wait and GetTickCount logic (src/ntdll/k32_sync_pure.h, used by kernel32.c).
$(OUT)/test_k32_sync_pure$(EXE_SUFFIX): test_k32_sync_pure.c minitest.h $(SRC)/k32_sync_pure.h $(SRC)/nt_types.h $(SRC)/ntstatus.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only code page conversion rules (src/ntdll/k32_str_pure.h, used by kernel32_str.c).
$(OUT)/test_k32_str_pure$(EXE_SUFFIX): test_k32_str_pure.c minitest.h $(SRC)/k32_str_pure.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only environment block rules (src/ntdll/k32_env_pure.h, used by kernel32_env.c).
$(OUT)/test_k32_env_pure$(EXE_SUFFIX): test_k32_env_pure.c minitest.h $(SRC)/k32_env_pure.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only priority, handle flag and PE binary type rules (src/ntdll/k32_sys_pure.h, used by kernel32_sys.c).
$(OUT)/test_k32_sys_pure$(EXE_SUFFIX): test_k32_sys_pure.c minitest.h $(SRC)/k32_sys_pure.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only file search, temp name and environment match rules (src/ntdll/k32_find_pure.h, used by kernel32_fs.c).
$(OUT)/test_k32_find_pure$(EXE_SUFFIX): test_k32_find_pure.c minitest.h $(SRC)/k32_find_pure.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only thread flag, exit code and suspend result rules (src/ntdll/k32_thread_pure.h, used by kernel32_thread.c).
$(OUT)/test_k32_thread_pure$(EXE_SUFFIX): test_k32_thread_pure.c minitest.h $(SRC)/k32_thread_pure.h $(SRC)/nt_types.h $(SRC)/ntstatus.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only file mapping flag rules (src/ntdll/k32_mm_pure.h, used by kernel32_mm.c).
$(OUT)/test_k32_mm_pure$(EXE_SUFFIX): test_k32_mm_pure.c minitest.h $(SRC)/k32_mm_pure.h $(SRC)/nt_types.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only advapi32 registry mapping (src/ntdll/advapi_pure.h, used by advapi32.c).
$(OUT)/test_advapi_pure$(EXE_SUFFIX): test_advapi_pure.c minitest.h $(SRC)/advapi_pure.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only loader address translation (src/ntdll/ldr_pure.h, used by ldr.c).
$(OUT)/test_ldr_pure$(EXE_SUFFIX): test_ldr_pure.c minitest.h $(SRC)/ldr_pure.h $(SRC)/pe_bounds.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# Header-only RVA-indexed module headers and exports for startup modules (src/ntdll/ldr_pure.h, used by ldr.c).
$(OUT)/test_ldr_mapped_pure$(EXE_SUFFIX): test_ldr_mapped_pure.c minitest.h $(SRC)/ldr_pure.h $(SRC)/pe_bounds.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

# unwind.c calls _NtRaiseException from except.c (RtlRaiseException).
# except.c also defines SetUnhandledExceptionFilter and UnhandledExceptionFilter,
# which the MinGW libkernel32 import library exports too. The objects come
# before the libraries on the link line, so the first (ntdll) definition wins;
# --allow-multiple-definition only silences the duplicate from the library.
$(OUT)/test_unwind$(EXE_SUFFIX): $(OUT)/unwind.o $(OUT)/except.o $(OUT)/test_unwind.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--allow-multiple-definition $^ -o $@

$(OUT)/test_unwind_pure$(EXE_SUFFIX): $(OUT)/unwind.o $(OUT)/except.o $(OUT)/test_unwind_pure.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--allow-multiple-definition $^ -o $@

$(OUT)/test_except$(EXE_SUFFIX): $(OUT)/except.o $(OUT)/unwind.o $(OUT)/test_except.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--allow-multiple-definition $^ -o $@

$(OUT)/test_except_pure$(EXE_SUFFIX): $(OUT)/except.o $(OUT)/unwind.o $(OUT)/test_except_pure.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--allow-multiple-definition $^ -o $@

# Coverage links (libgcov) pull in a CRT _errno that clashes with ntdll_runtime.o;
# the first definition (ours, listed first) wins, as for unwind and except above.
comma := ,
COV_LINK := $(if $(COV_FLAGS),-Wl$(comma)--allow-multiple-definition)

# ntdll_runtime.c replaces libc string and allocator functions. Keep gcc from
# turning its loops back into calls to memcpy/memset.
$(OUT)/test_runtime$(EXE_SUFFIX): $(OUT)/ntdll_runtime.o $(OUT)/test_runtime.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(COV_LINK) $^ -o $@

# -Wno-error=attributes: ntdll_runtime.c declares "int errno" and "_errno", which
# clash with the dllimport attribute on MinGW's _errno (see docs/review/ntdll.md).
$(OUT)/ntdll_runtime.o: EXTRA_CFLAGS := -fno-builtin -fno-tree-loop-distribute-patterns -Wno-error=attributes

# win32k model (src/ntdll/win32k): one object per source, shared by the w32k tests.
W32K_SRCS := w32k_core.c w32k_msg.c w32k_input.c w32k_sys.c w32k_gdi.c
W32K_OBJS := $(addprefix $(OUT)/,$(W32K_SRCS:.c=.o))
$(OUT)/w32k_%.o: $(W32K)/w32k_%.c $(W32K)/w32k.h $(W32K)/w32k_int.h $(W32K)/w32k_evdev.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/test_w32k_%$(EXE_SUFFIX): test_w32k_%.c minitest.h $(W32K_OBJS) $(W32K)/w32k.h $(W32K)/w32k_evdev.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(W32K_OBJS) -o $@ $(LDFLAGS)

$(OUT)/%.o: $(SRC)/%.c | $(OUT)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/test_%.o: test_%.c minitest.h | $(OUT)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(CPPFLAGS) -c $< -o $@

# ntdll_runtime.c replaces malloc, calloc and free for the whole process. ASan
# interposes the same symbols with its own allocator and aborts on the
# calloc-overflow case that test_runtime checks (LNX-06), so these objects are
# built without sanitizers. The test source is the same in both builds.
NOSAN_CFLAGS := $(filter-out -fsanitize% -fno-sanitize%,$(CFLAGS))
$(OUT)/ntdll_runtime.o $(OUT)/test_runtime.o $(OUT)/test_runtime$(EXE_SUFFIX): CFLAGS := $(NOSAN_CFLAGS)

# The runtime test calls calloc and malloc by name. Without -fno-builtin gcc
# treats them as built-ins and the overflow checks are not exercised.
$(OUT)/test_runtime.o: EXTRA_CFLAGS := -fno-builtin

$(OUT):
	mkdir -p $(OUT)

# Prints the binaries "test" runs, so tools/c_coverage.sh can run them without
# stopping at the first failure.
print-run-bins:
	@echo $(RUN_BINS)

clean:
	rm -rf $(OUT)
