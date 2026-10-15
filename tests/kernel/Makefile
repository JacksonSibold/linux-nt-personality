CC := x86_64-w64-mingw32-gcc
CFLAGS := -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector
SRCDIR := src
BINDIR := bin

# PE sources excluding the ELF twin
PE_SRCS := $(filter-out $(SRCDIR)/pmu_elf.c,$(wildcard $(SRCDIR)/*.c))
BINS := $(patsubst $(SRCDIR)/%.c,$(BINDIR)/%.exe,$(PE_SRCS))
ELF_BIN := $(BINDIR)/pmu_elf

all: $(BINS) $(ELF_BIN)

$(BINDIR)/%.exe: $(SRCDIR)/%.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(ELF_BIN): $(SRCDIR)/pmu_elf.c | $(BINDIR)
	gcc -O2 -static -o $@ $< 2>/dev/null || gcc -O2 -o $@ $<

$(BINDIR):
	mkdir -p $@

$(BINDIR):
	mkdir -p $@

# S2 (console milestone): exes that import from ntdll.dll. Not part of `all`:
# they need src/ntdll/out/ntdll.def (make -C src/ntdll first).
NTDLL_DEF := ../../src/ntdll/out/ntdll.def
S2_EXES := $(BINDIR)/console_s2.exe

$(BINDIR)/libntdll_imp.a: $(NTDLL_DEF) | $(BINDIR)
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D ntdll.dll

$(BINDIR)/console_s2.exe: s2/console_s2.c $(BINDIR)/libntdll_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lntdll_imp

s2: $(S2_EXES)

$(BINDIR)/libkernel32_imp.a: ../../src/ntdll/kernel32.def | $(BINDIR)
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D kernel32.dll

$(BINDIR)/console_s3.exe: s3/console_s3.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

s3: $(BINDIR)/console_s3.exe

# S6: GetModuleHandleA("kernel32.dll") and GetProcAddress on the startup module list.
# Uses the same kernel32 import library as S3.
$(BINDIR)/console_s6.exe: s6/console_s6.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

s6: $(BINDIR)/console_s6.exe

# S7: a CreateThread thread reads its TEB through %gs (NtCreateThreadEx, src/kernel/nt/proc_thread.c).
$(BINDIR)/console_s7.exe: s7/console_s7.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

s7: $(BINDIR)/console_s7.exe

# S8: the exe sees its own command line and environment through the PEB (binfmt_pe.c).
$(BINDIR)/console_s8.exe: s8/console_s8.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

s8: $(BINDIR)/console_s8.exe

# S9: the kernel32 environment API (GetEnvironmentVariable, SetEnvironmentVariable,
# ExpandEnvironmentStrings, GetCommandLineA) in a booted kernel. Needs the new kernel32 exports.
$(BINDIR)/console_s9.exe: s9/console_s9.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

s9: $(BINDIR)/console_s9.exe

# S10: the standard handles (GetStdHandle, ReadFile on stdin, WriteFile on stdout and
# stderr, GetFileType) in a booted kernel. Needs the kernel's std-handle install.
$(BINDIR)/console_s10.exe: s10/console_s10.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

s10: $(BINDIR)/console_s10.exe

# S11: process exit releases NT handles (io.c ntio_release_owned_handles). console_s11 is
# started 1500 times from the step's shell loop; pipe_s11 reads a pipe standard input and
# checks GetFileType == FILE_TYPE_PIPE. Both need the kernel's exit release.
$(BINDIR)/console_s11.exe: s11/console_s11.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

$(BINDIR)/pipe_s11.exe: s11/pipe_s11.c $(BINDIR)/libkernel32_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lkernel32_imp

s11: $(BINDIR)/console_s11.exe $(BINDIR)/pipe_s11.exe

$(BINDIR)/libmsvcrt_imp.a: ../../src/ntdll/msvcrt.def | $(BINDIR)
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D msvcrt.dll

$(BINDIR)/console_s4.exe: s4/console_s4.c $(BINDIR)/libmsvcrt_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lmsvcrt_imp

s4: $(BINDIR)/console_s4.exe

# S5: an exe importing from a DLL that the initramfs does not ship. The strict
# loader must fail it with STATUS_DLL_NOT_FOUND (0xC0000135), not run it.
$(BINDIR)/libmissing_s5_imp.a: s5/missing_s5.def | $(BINDIR)
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D missing_s5.dll

$(BINDIR)/console_s5.exe: s5/console_s5.c $(BINDIR)/libmissing_s5_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lmissing_s5_imp

s5: $(BINDIR)/console_s5.exe

clean:
	rm -rf $(BINDIR)/*.exe

.PHONY: all clean
