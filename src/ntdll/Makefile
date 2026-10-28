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
C_SRCS := ntdll_main.c ldr.c except.c unwind.c seh.c ntdll_runtime.c ntdll_heap.c ntdll_sync.c ntstatus_to_dos.c

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

$(OUT)/ntdll.def: $(DEF_GEN) $(SYSCALLS) $(SRC)/ntdll_exports.txt | $(OUT)
	$(PYTHON) "$(DEF_GEN)" "$(SYSCALLS)" "$@"

$(OUT)/%.o: $(SRC)/%.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ntdll's own malloc calls RtlAllocateHeap in this module; the other DLLs import it (see ntdll_runtime.c).
$(OUT)/ntdll_runtime.o: CPPFLAGS += -DNTDLL_BUILD

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

# kernel32.dll (S3 of the console milestone): kernel32.c with the runtime helpers
# (memcpy, strlen, ...) that ntdll.dll does not export, importing ntdll.dll.
K32_OBJS := $(OUT)/kernel32.o $(OUT)/kernel32_mm.o $(OUT)/kernel32_thread.o $(OUT)/kernel32_fs.o $(OUT)/kernel32_misc.o $(OUT)/kernel32_sys.o $(OUT)/kernel32_str.o $(OUT)/kernel32_env.o $(OUT)/kernel32_global.o $(OUT)/kernel32_time.o $(OUT)/kernel32_heap.o $(OUT)/kernel32_sync.o $(OUT)/kernel32_w.o $(OUT)/kernel32_obj.o $(OUT)/kernel32_x.o $(OUT)/kernel32_nls.o $(OUT)/kernel32_y.o $(OUT)/kernel32_ini.o $(OUT)/kernel32_res.o $(OUT)/k32_runtime.o

$(OUT)/k32_runtime.o: ntdll_runtime.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/libntdll_imp.a: $(OUT)/ntdll.def
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D ntdll.dll

$(OUT)/kernel32.dll: $(K32_OBJS) kernel32.def $(OUT)/libntdll_imp.a
	$(CC) -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined -o $@ $(K32_OBJS) kernel32.def -L$(OUT) -lntdll_imp -lgcc

.PHONY: kernel32
kernel32: $(OUT)/kernel32.dll

# msvcrt.dll (S4): msvcrt.c imports kernel32 and ntdll; the runtime helpers come from k32_runtime.o.
$(OUT)/libkernel32_imp.a: kernel32.def
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D kernel32.dll

$(OUT)/msvcrt.dll: $(OUT)/msvcrt.o msvcrt.def $(OUT)/libkernel32_imp.a $(OUT)/libntdll_imp.a
	$(CC) -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined -o $@ $(OUT)/msvcrt.o msvcrt.def -L$(OUT) -lkernel32_imp -lntdll_imp -lgcc

.PHONY: msvcrt
msvcrt: $(OUT)/msvcrt.dll

# ucrtbase.dll: the Universal C Runtime. src/crt holds the library as headers (host-tested in tests/crt-host);
# ucrtbase.c puts it on kernel32. ucrtbase.def and ucrtbase_gen.inc come from build/gen_ucrtbase.py.
CRT_HDRS := $(wildcard $(ROOT)/src/crt/*.h) $(wildcard ucrtbase*.inc)

$(OUT)/ucrtbase.o: ucrtbase.c $(CRT_HDRS) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(ROOT)/src/crt -Wno-unused-function -Wno-array-bounds -c $< -o $@

$(OUT)/ucrtbase.dll: $(OUT)/ucrtbase.o $(OUT)/k32_runtime.o ucrtbase.def $(OUT)/libkernel32_imp.a $(OUT)/libntdll_imp.a
	$(CC) -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined -o $@ $(OUT)/ucrtbase.o $(OUT)/k32_runtime.o ucrtbase.def -L$(OUT) -lkernel32_imp -lntdll_imp -lgcc

.PHONY: ucrtbase
ucrtbase: $(OUT)/ucrtbase.dll
# advapi32.dll: the registry subset the differential probes import (advapi32.def),
# on the NT registry services in ntdll.dll. Uses the same runtime helpers as kernel32.
$(OUT)/advapi32.o: advapi32.c advapi_pure.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OUT)/advapi32.dll: $(OUT)/advapi32.o $(OUT)/k32_runtime.o advapi32.def $(OUT)/libntdll_imp.a
	$(CC) -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined -o $@ $(OUT)/advapi32.o $(OUT)/k32_runtime.o advapi32.def -L$(OUT) -lntdll_imp -lgcc
.PHONY: advapi32
advapi32: $(OUT)/advapi32.dll
# user32.dll: the wsprintf subset (user32.def) the probes import. user32.c is self-contained
# (it defines its own ___chkstk_ms), so k32_runtime.o is not linked: it would clash.
$(OUT)/user32.o: user32.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
# win32k model (src/ntdll/win32k): the service layer behind user32 and gdi32. One object per
# source; the shims (user32_win32k.c, gdi32_win32k.c) convert the Win32 ABI to the model.
# The model is linked once, into win32u.dll (win32u.c, exports from out/win32u.def); user32.dll
# and gdi32.dll import it, so both see the same windows, handles and DCs (decision 0076).
W32K_CORE_OBJS := $(addprefix $(OUT)/,w32k_core.o w32k_msg.o w32k_input.o w32k_sys.o w32k_gdi.o w32k_clip.o w32k_font.o w32k_region.o w32k_win.o w32k_rawinput.o)
$(OUT)/w32k_%.o: win32k/w32k_%.c win32k/w32k.h win32k/w32k_int.h win32k/w32k_evdev.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OUT)/user32_win32k.o: user32_win32k.c win32k/w32k.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OUT)/user32_ext.o: user32_ext.c user32_pure.h win32k/w32k.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OUT)/gdi32_win32k.o: gdi32_win32k.c win32k/w32k.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(OUT)/win32u.o: win32u.c win32k/w32k.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/win32u.def: $(ROOT)/build/gen_win32u_def.py win32k/w32k.h | $(OUT)
	$(PYTHON) $(ROOT)/build/gen_win32u_def.py win32k/w32k.h $@
$(OUT)/win32u.dll: $(OUT)/win32u.o $(W32K_CORE_OBJS) $(OUT)/win32u.def $(OUT)/libntdll_imp.a
	$(CC) -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined -o $@ $(OUT)/win32u.o $(W32K_CORE_OBJS) $(OUT)/win32u.def -L$(OUT) -lntdll_imp -lgcc
$(OUT)/libwin32u_imp.a: $(OUT)/win32u.def
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D win32u.dll
.PHONY: win32u
win32u: $(OUT)/win32u.dll

$(OUT)/user32.dll: $(OUT)/user32.o $(OUT)/user32_win32k.o $(OUT)/user32_ext.o user32.def $(OUT)/libntdll_imp.a $(OUT)/libkernel32_imp.a $(OUT)/libwin32u_imp.a
	$(CC) -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined -o $@ $(OUT)/user32.o $(OUT)/user32_win32k.o $(OUT)/user32_ext.o user32.def -L$(OUT) -lkernel32_imp -lntdll_imp -lwin32u_imp -lgcc
.PHONY: user32
user32: $(OUT)/user32.dll

# gdi32.dll: GDI objects and blits (gdi32.def) over the same win32k model as user32, through win32u.dll.
$(OUT)/gdi32.dll: $(OUT)/gdi32_win32k.o gdi32.def $(OUT)/libntdll_imp.a $(OUT)/libwin32u_imp.a
	$(CC) -nostdlib -shared -fno-use-linker-plugin -Wl,-e,DllMainCRTStartup -Wl,--no-undefined -o $@ $(OUT)/gdi32_win32k.o gdi32.def -L$(OUT) -lntdll_imp -lwin32u_imp -lgcc
.PHONY: gdi32
gdi32: $(OUT)/gdi32.dll
