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

clean:
	rm -rf $(BINDIR)/*.exe

.PHONY: all clean
