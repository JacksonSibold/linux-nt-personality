# tests/pe-host - runs this repository's PE programs in a Linux process (ntpers_run), on the kernel's NT code built for
# user space. See README.md. Linux only: it needs Syscall User Dispatch (Linux 5.11 or later, x86-64).
KH      := ../kernel-host
NT      := ../../src/kernel/nt
LOADER  := ../../src/loader
SHIM    := $(KH)/shim
BUILD   := build
KBUILD  := $(KH)/build
NTDLL   := ../../src/ntdll
ROOT    := $(BUILD)/root

.DEFAULT_GOAL := all
CC      ?= gcc
CFLAGS  := -std=gnu11 -g -O0 -Wall -Wextra -DUSERSPACE_KSHIM -include $(SHIM)/linux/khost_prelude.h \
           -iquote $(NT) -iquote $(LOADER) -iquote $(KBUILD) -I$(SHIM)/linux
# binfmt_pe.c includes "../../arch/x86/nt/nt_internal.h" when CONFIG_NT_PERSONALITY is set; shim/a/b makes that path resolve
# to the stand-in under shim/arch.
BOOT_CFLAGS := $(CFLAGS) -iquote shim/a/b -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter

# The NT modules, as tests/kernel-host builds them.
MODS := security registry object handle nt_obj sync kshim mm io iocp proc_thread nt_services job port debugobj ntstatus_kernel \
        pe_parse pe_prot pe_text fs_kshim khost_linux
# io.c and nt_obj.c each define NtClose for their own handle table in the user-space build. The runner needs both, so
# each copy gets a name of its own and ntpers_run.c supplies the service that tries one and then the other.
MOD_OBJS := $(addprefix $(KBUILD)/,$(addsuffix .o,$(filter-out io nt_obj,$(MODS)))) $(BUILD)/io_r.o $(BUILD)/nt_obj_r.o

$(BUILD)/io_r.o: modules | $(BUILD)
	objcopy --redefine-sym nt_svc_NtClose=pehost_io_NtClose $(KBUILD)/io.o $@

$(BUILD)/nt_obj_r.o: modules | $(BUILD)
	objcopy --redefine-sym nt_svc_NtClose=pehost_ob_NtClose $(KBUILD)/nt_obj.o $@

.PHONY: all test clean modules
all: $(BUILD)/ntpers_run

modules:
	$(MAKE) -C $(KH) $(addprefix build/,$(addsuffix .o,$(MODS)))

$(BUILD):
	mkdir -p $@

$(BUILD)/ntpers_run.o: ntpers_run.c pe_host.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pe_boot.o: pe_boot.c pe_host.h ../../src/kernel/fs/binfmt_pe.c ../../src/kernel/fs/kshim.h | $(BUILD)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/ntpers_run: modules $(BUILD)/ntpers_run.o $(BUILD)/pe_boot.o $(BUILD)/io_r.o $(BUILD)/nt_obj_r.o
	$(CC) -fno-use-linker-plugin -o $@ $(BUILD)/ntpers_run.o $(BUILD)/pe_boot.o $(MOD_OBJS) -lpthread

# The root directory NT paths resolve under: the repository's DLLs where a Windows system has them.
DLLS := ntdll kernel32 kernelbase msvcrt ucrtbase advapi32 vcruntime140 vcruntime140_1
.PHONY: root dlls
dlls:
	$(MAKE) -C $(NTDLL) all kernel32 msvcrt ucrtbase advapi32 vcruntime140
	-$(MAKE) -C $(NTDLL) kernelbase

root: dlls
	mkdir -p $(ROOT)/windows/system32 $(ROOT)/tmp
	for d in $(DLLS); do if [ -f $(NTDLL)/out/$$d.dll ]; then cp $(NTDLL)/out/$$d.dll $(ROOT)/windows/system32/; fi; done
	cp $(NTDLL)/out/ntdll.dll $(ROOT)/ntdll.dll

clean:
	rm -rf $(BUILD)
