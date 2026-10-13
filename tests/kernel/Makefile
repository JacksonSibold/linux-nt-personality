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

$(BINDIR)/libmsvcrt_imp.a: ../../src/ntdll/msvcrt.def | $(BINDIR)
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D msvcrt.dll

$(BINDIR)/console_s4.exe: s4/console_s4.c $(BINDIR)/libmsvcrt_imp.a
	$(CC) -nostdlib -Wl,-e,_start -Wl,--no-seh -Wl,--no-insert-timestamp -Os -s -ffreestanding -fno-builtin -fno-stack-protector -o $@ $< -L$(BINDIR) -lmsvcrt_imp

s4: $(BINDIR)/console_s4.exe

clean:
	rm -rf $(BINDIR)/*.exe

.PHONY: all clean
