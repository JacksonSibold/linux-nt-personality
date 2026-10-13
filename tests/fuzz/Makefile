# Deterministic mutation fuzzer for src/loader (pe_parse.c). Plain gcc, no
# sanitizers, same conventions as tests/host/Makefile.
#
#   make -C tests/fuzz smoke          2000 iterations per seed file (CI, run_all_checks.sh)
#   make -C tests/fuzz fuzz           100000 iterations per seed file
#   make -C tests/fuzz ITERS=N SEED=S run   custom iteration count or base seed
#   make -C tests/fuzz check-one FILE=out/fail-....bin   re-run one saved input
#   make -C tests/fuzz minimise FILE=out/fail-....bin    shrink a saved input
#
# Seeds: synthesized images from tests/host/pe_image.c (written by fuzz_pe
# --gen-seeds) plus the MinGW fixtures from tests/host/fixtures, when they can
# be built. Failing inputs go to tests/fuzz/out/fail-*.bin.

# Under MSYS2 make does not see the OS variable, so also check uname.
UNAME_S := $(shell uname -s 2>/dev/null)
ifneq (,$(filter Windows_NT,$(OS))$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
# Same exports as tests/host/Makefile (see the toolchain notes in TOOLCHAIN.md).
TEMP_DIR ?= $(if $(TEMP),$(TEMP),$(if $(TMP),$(TMP),$(if $(LOCALAPPDATA),$(shell cygpath -m "$(LOCALAPPDATA)/Temp"),$(shell cygpath -m /tmp))))
export TMP    := $(TEMP_DIR)
export TEMP   := $(TEMP_DIR)
export TMPDIR := $(shell cygpath -u "$(TEMP_DIR)")
EXE_SUFFIX := .exe
else
EXE_SUFFIX :=
endif

CC      := gcc
SRC     := ../../src/loader
HOST    := ../host
OUT     := out
SEEDS   := $(OUT)/seeds
FUZZ    := $(OUT)/fuzz_pe$(EXE_SUFFIX)
ITERS   ?= 2000
SEED    ?= 1
FILE    ?=
LDFLAGS := -fno-use-linker-plugin

CPPFLAGS := -I$(SRC) -I$(HOST)
CFLAGS   := -std=c11 -g -O1 -Wall -Wextra -Werror

OBJS := $(OUT)/fuzz_pe.o $(OUT)/fuzz_plat.o $(OUT)/pe_parse.o $(OUT)/pe_image.o

# Fixtures built by tests/host (MinGW PE images, read-only here).
FIX_NAMES := minimal.exe exports.dll

.PHONY: all smoke fuzz run seeds check-one minimise clean

all: smoke

smoke:
	$(MAKE) run ITERS=2000

fuzz:
	$(MAKE) run ITERS=100000

run: $(FUZZ) seeds
	./$(FUZZ) --seed $(SEED) --iters $(ITERS) $(SEEDS)/synth_base.bin \
	    $(SEEDS)/synth_import.bin $(SEEDS)/synth_reloc.bin \
	    $(SEEDS)/synth_exception.bin $(SEEDS)/synth_all.bin \
	    $(foreach f,$(FIX_NAMES),$(if $(wildcard $(SEEDS)/$(f)),$(SEEDS)/$(f),))

# Writes the synthesized seeds, then copies the host fixtures that exist.
# A fixture that cannot be built is skipped with a note, not treated as an error.
seeds: $(FUZZ)
	mkdir -p $(SEEDS)
	./$(FUZZ) --gen-seeds $(SEEDS)
	-$(MAKE) -C $(HOST) out/fixtures/minimal.exe out/fixtures/exports.dll
	@for f in $(FIX_NAMES); do \
	    if [ -f $(HOST)/out/fixtures/$$f ]; then cp $(HOST)/out/fixtures/$$f $(SEEDS)/; \
	    else echo "seeds: fixture $$f not present, skipped"; fi; \
	done

check-one: $(FUZZ)
	./$(FUZZ) --check-one "$(FILE)"

minimise: $(FUZZ)
	./$(FUZZ) --minimise "$(FILE)"

$(FUZZ): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(OUT)/fuzz_pe.o: fuzz_pe.c $(SRC)/pe_parse.h $(SRC)/pe_format.h $(HOST)/pe_image.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/fuzz_plat.o: fuzz_plat.c fuzz_plat.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/pe_parse.o: $(SRC)/pe_parse.c $(SRC)/pe_parse.h $(SRC)/pe_format.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT)/pe_image.o: $(HOST)/pe_image.c $(HOST)/pe_image.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)
