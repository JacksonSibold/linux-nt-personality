# Makefile for ntdll.dll (x86_64 PE, MinGW-w64 target).
#
#   make -C src/ntdll            build out/ntdll.dll (and out/ntdll.def)
#   make -C src/ntdll check      build, then compare exports with the .def
#                                (tests/ntdll-host/check_exports.sh) and check
#                                that LdrLoadDll is not the stub
#                                (tests/ntdll-host/check_ldr_stub.sh); build-only
#   make -C src/ntdll clean      remove out/
#
# make does not import TMP from the MSYS2 shell on the development machine;
# if the link fails with "Cannot create temporary file", pass TMP=<dir>.
#
# Do NOT run out/ntdll.dll or any exe built here: the Windows Application
# Control policy on the development machine blocks freshly built binaries
# (docs/review/ntdll.md, MAIN-1). Inspect with objdump/nm only.
#
# Toolchain: MSYS2 UCRT64 gcc (x86_64-w64-mingw32), see the machine notes in
# C:/Users/Jackson/repos/TOOLCHAIN.md. Python 3 is needed for the .def generator.
#
# Required flags and why:
#   -nostdlib                 No libc and no CRT startup. ntdll_runtime.c
#                             supplies the string and allocator functions and
#                             ntdll_main.c the DLL entry.
#   -shared                   Produce a PE DLL (IMAGE_FILE_DLL).
#   -ffreestanding            Hosted-library assumptions are off.
#   -fno-builtin              Keep gcc from turning the loops in ntdll_runtime.c
#   -fno-tree-loop-distribute-patterns
#                             back into calls to memcpy/memset (same reason as
#                             tests/ntdll-host/Makefile).
#   -Wl,-e,DllMainCRTStartup  Entry point. Named explicitly because no CRT
#                             provides _start.
#   -fno-use-linker-plugin    Workaround for liblto_plugin.dll failing to load
#                             on the MSYS2 gcc (TOOLCHAIN.md). Harmless elsewhere.
#   -lgcc                     Compiler helper routines, linked explicitly
#                             because -nostdlib drops the default libraries.
#
# Inputs:
#   out/ntdll.def             generated from src/ntdll/syscall_table.def by
#                             build/gen_ntdll_def.py (the export list).
#   build/generated_stubs.S   generated from syscall_table.def by
#                             build/gen_stubs.py (the Nt*/Zw* syscall stubs).
#                             src/ntdll/syscall_stub.S is NOT linked: it defines
#                             the same symbols and would collide.

ROOT := ../..
SRC  := .
OUT  := out
PYTHON ?= python3
CC   := x86_64-w64-mingw32-gcc

# Windows: the link step needs a writable temp dir (TOOLCHAIN.md). The recipe
# shell does not reliably inherit these, so they are exported here.
UNAME_S := $(shell uname -s 2>/dev/null)
# make does not import TMP/LOCALAPPDATA from the MSYS2 shell here, so the
# default is a directory inside out/. Override on the command line if needed:
#   make -C src/ntdll TMP=C:/path/to/temp
ifneq (,$(filter Windows_NT,$(OS))$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
TEMP_DIR ?= $(shell mkdir -p $(OUT) && cygpath -m "$(CURDIR)/$(OUT)")
export TMP    := $(TEMP_DIR)
export TEMP   := $(TEMP_DIR)
export TMPDIR := $(shell cygpath -u "$(TEMP_DIR)")
endif

DEF_GEN   := $(ROOT)/build/gen_ntdll_def.py
SYSCALLS  := $(SRC)/syscall_table.def
STUBS_S   := $(ROOT)/build/generated_stubs.S

# Sources of the MinGW ntdll. Each file is guarded by __MINGW32__ where it
# needs to be (ldr.c: real loader only in the non-MinGW build, stubs here).
C_SRCS := ntdll_main.c ldr.c except.c unwind.c ntdll_runtime.c ntstatus_to_dos.c

CPPFLAGS := -I$(SRC) -I$(ROOT)/src/loader -DPE_FREESTANDING
CFLAGS   := -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
            -fno-tree-loop-distribute-patterns -fno-stack-protector
LDFLAGS  := -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined
LDLIBS   := -lgcc

# PE parser shared with the host tests (src/loader). ldr.c calls pe_* for
# the module headers; without it the link leaves them undefined.
LOADER_PE := $(ROOT)/src/loader/pe_parse.c

OBJS := $(addprefix $(OUT)/,$(C_SRCS:.c=.o)) $(OUT)/generated_stubs.o $(OUT)/pe_parse.o

.PHONY: all check clean

all: $(OUT)/ntdll.dll

$(OUT)/ntdll.def: $(DEF_GEN) $(SYSCALLS) | $(OUT)
	$(PYTHON) "$(DEF_GEN)" "$(SYSCALLS)" "$@"

$(OUT)/%.o: $(SRC)/%.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/pe_parse.o: $(LOADER_PE) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/generated_stubs.o: $(STUBS_S) | $(OUT)
	$(CC) -c $< -o $@

$(OUT)/ntdll.dll: $(OBJS) $(OUT)/ntdll.def
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(OUT)/ntdll.def $(LDLIBS)

# check: compare the DLL's export names with out/ntdll.def (build-only; the DLL
# is never loaded or run). Not wired into CI.
check: $(OUT)/ntdll.dll $(OUT)/ntdll.def
	$(ROOT)/tests/ntdll-host/check_exports.sh $(OUT)/ntdll.dll $(OUT)/ntdll.def
	$(ROOT)/tests/ntdll-host/check_ldr_stub.sh $(OUT)/ntdll.dll

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)
