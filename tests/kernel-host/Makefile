# tests/kernel-host/Makefile - build and run the src/kernel/nt unit tests on
# the Windows host (UCRT gcc) against the userspace kshim (USERSPACE_KSHIM).
# See README.md for the module-by-module status and the shim layout.
#
# Toolchain: C:/Users/Jackson/repos/TOOLCHAIN.md. Run make from a shell that
# already has /c/msys64/ucrt64/bin on PATH.

# Under MSYS2 make does not see the OS variable, so also check uname.
UNAME_S := $(shell uname -s 2>/dev/null)
ifneq (,$(filter Windows_NT,$(OS))$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
# TEMP_DIR is taken from the environment if set, else TEMP, TMP,
# LOCALAPPDATA/Temp, and finally MSYS /tmp (which maps to the Windows temp
# directory). Override it on the command line if needed. The recipe shell
# does not reliably inherit TMP/TEMP, hence the exports here.
TEMP_DIR ?= $(if $(TEMP),$(TEMP),$(if $(TMP),$(TMP),$(if $(LOCALAPPDATA),$(shell cygpath -m "$(LOCALAPPDATA)/Temp"),$(shell cygpath -m /tmp))))
export TMP    := $(TEMP_DIR)
export TEMP   := $(TEMP_DIR)
export TMPDIR := $(shell cygpath -u "$(TEMP_DIR)")
endif

# KHOST_OS selects the host shim. windows: the UCRT shim (shim/*.c, ws2_32,
# VirtualAlloc-based mmap). linux: glibc provides the POSIX names, so the shim is
# shim/linux (a prelude plus the scratch-root hook) and no ws2_32 is linked.
# Detected from uname; override on the command line if needed.
ifneq (,$(filter Windows_NT,$(OS))$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
KHOST_OS ?= windows
else
KHOST_OS ?= linux
endif

CC      := gcc
NT      := ../../src/kernel/nt
NTDLL   := ../../src/ntdll
LOADER  := ../../src/loader
FS      := ../../src/kernel/fs
SHIM    := shim
# COVERAGE=1: gcov instrumentation, -O0, objects in build-cov/ so the
# instrumented and plain builds never share .o files. See tools/c_coverage.sh.
COVERAGE ?= 0
ifeq ($(COVERAGE),1)
BUILD   := build-cov
COV_FLAGS := --coverage
COV_LIBS  := -lgcov
else
BUILD   := build
COV_FLAGS :=
COV_LIBS  :=
endif
# SANITIZE=address (ASan+UBSan) or SANITIZE=thread (TSan): sanitizer builds in
# build-asan/ or build-tsan/, so the plain objects are never mixed with them.
# Linux gcc only; the MSYS2 gcc on the development machine has no libasan/libtsan.
SANITIZE ?=
ifeq ($(SANITIZE),address)
BUILD    := build-asan
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
else ifeq ($(SANITIZE),thread)
BUILD    := build-tsan
SAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer
else
SAN_FLAGS :=
endif
NTSTATUS_INC := $(BUILD)/ntstatus_table.inc

# OPT defaults to -O0. Use OPT=-O2 with a separate BUILD=... directory to
# expose optimiser-dependent uninitialised reads (-Wuninitialized is on).
# Coverage builds always use -O0 so that line counts map to source lines.
OPT     ?= -O0
ifeq ($(COVERAGE),1)
OPT     := -O0
endif
WARN    := -Wall -Wextra -Wuninitialized
DEPFLAGS := -MMD -MP
ifeq ($(KHOST_OS),linux)
PRELUDE   := $(SHIM)/linux/khost_prelude.h
SHIM_INC  := -I$(SHIM)/linux
HOST_LIBS := -lpthread
# Host-only POSIX pieces on Linux: the scratch-root hook only (see shim/linux/).
SHIM_OBJS := $(BUILD)/khost_linux.o
else
PRELUDE   := $(SHIM)/khost_prelude.h
SHIM_INC  := -I$(SHIM)
HOST_LIBS := -lpthread -lws2_32
# Host-only POSIX primitives (see shim/).
SHIM_OBJS := $(BUILD)/mman_win.o $(BUILD)/posix_host.o $(BUILD)/posix_raw.o
endif

CFLAGS  := -std=gnu11 -g $(OPT) $(WARN) $(DEPFLAGS) $(COV_FLAGS) $(SAN_FLAGS) -DUSERSPACE_KSHIM \
           -include $(PRELUDE) \
           -iquote $(NT) -iquote $(LOADER) -iquote $(BUILD) $(SHIM_INC)
LDLIBS  := $(HOST_LIBS) $(COV_LIBS)
LDFLAGS := -fno-use-linker-plugin $(COV_FLAGS) $(SAN_FLAGS)

# Test programs and the module objects each one links against.
TESTS := port_test job_test security_extra_test object_test object_extra_test security_test registry_test i2_test io_test mm_test sync_test sync_extra_test proc_thread_test handle_edge_test registry_ops_test system_test process_params_test handle_inherit_test proc_create_args_test image_plan_test image_reloc_test debugobj_test thread_exit_test exc_deliver_test driver_svcdb_test

# Tests kept in this directory (not in src/kernel/nt). Their sources are $(t).c here.
LOCAL_TESTS := port_test job_test handle_edge_test registry_ops_test process_params_test handle_inherit_test proc_create_args_test image_plan_test image_reloc_test debugobj_test thread_exit_test exc_deliver_test driver_svcdb_test

# Concurrency stress programs (see README.md "Stress tests"). Run by "stress",
# never by "check". Sources are in this directory, like LOCAL_TESTS.
STRESS_TESTS := stress_handle_test stress_object_test stress_sync_test stress_token_test stress_section_test stress_namespace_test stress_sd_test stress_registry_test stress_timer_test
LOCAL_TESTS += $(STRESS_TESTS)

# binfmt_pe_test (src/kernel/fs): the PE loader's unit test. It #includes
# binfmt_pe.c against the userspace model in src/kernel/fs/kshim.h, which
# models the Linux x86-64 LP64 kernel API and uses glibc's <sys/*.h> and
# linux/types.h. So it is built only where those exist: the Linux host (the
# dev guest). On Windows (LLP64, UCRT) it is not part of TESTS.
FS_TESTS := binfmt_pe_test
ifeq ($(KHOST_OS),linux)
TESTS += $(FS_TESTS)
endif

# Built but not run by "check" (none at present).
BUILD_ONLY :=
ALL_PROGS := $(TESTS) $(BUILD_ONLY) $(STRESS_TESTS)

object_test_OBJS      := security object handle kshim
object_extra_test_OBJS := security object handle nt_obj sync kshim
sync_test_OBJS        := security sync kshim object handle nt_obj
sync_extra_test_OBJS  := security object handle nt_obj sync kshim
port_test_OBJS        := security object handle nt_obj sync kshim port
security_extra_test_OBJS      := security object handle kshim
security_test_OBJS    := security object handle kshim
mm_test_OBJS          := mm security object handle kshim io iocp nt_obj_weak sync pe_parse
registry_test_OBJS    := security registry object handle nt_obj sync kshim
io_test_OBJS          := security io object handle iocp nt_obj_weak sync kshim
proc_thread_test_OBJS := security proc_thread object handle nt_obj sync nt_services kshim
i2_test_OBJS          := security object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel
system_test_OBJS        := security object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel
handle_edge_test_OBJS := security object handle kshim
process_params_test_OBJS :=
handle_inherit_test_OBJS := security object handle kshim
proc_create_args_test_OBJS :=
image_plan_test_OBJS := pe_parse
image_reloc_test_OBJS := pe_parse
job_test_OBJS := security object handle nt_obj sync kshim job
debugobj_test_OBJS := security proc_thread object handle nt_obj sync nt_services kshim debugobj
thread_exit_test_OBJS := security proc_thread object handle nt_obj sync nt_services kshim
registry_ops_test_OBJS := security registry object handle nt_obj sync kshim
driver_svcdb_test_OBJS := security registry object handle nt_obj sync kshim drvhost_svcdb
stress_handle_test_OBJS := security object handle kshim
stress_object_test_OBJS := security object handle kshim
stress_sync_test_OBJS := security object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel
stress_token_test_OBJS := security object handle kshim
stress_section_test_OBJS := mm security object handle io iocp nt_obj_weak sync kshim pe_parse
stress_namespace_test_OBJS := security object handle nt_obj sync kshim
stress_sd_test_OBJS := security object handle nt_obj sync kshim
stress_registry_test_OBJS := security registry object handle nt_obj sync kshim
stress_timer_test_OBJS := security object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel
# Loader units linked into binfmt_pe_test: PE protection mapping, UTF-16 and
# command-line/environment builders, and the userspace kernel model's globals.
binfmt_pe_test_OBJS := pe_parse pe_prot pe_text fs_kshim

# Shim self-tests (shim/mman_test.c). Run by "check"; not kernel module tests.
SHIM_TESTS := mman_test

PROGS := $(addprefix $(BUILD)/,$(addsuffix .exe,$(ALL_PROGS) $(SHIM_TESTS)))

.PHONY: all check check-quick check-full clean
all: $(PROGS)

$(BUILD):
	mkdir -p $(BUILD)

-include $(wildcard $(BUILD)/*.d)

# Host shim units. These are built without the prelude and, for the POSIX
# wrappers, with only the shim include directory.
$(BUILD)/posix_raw.o: $(SHIM)/posix_raw.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -c $< -o $@

$(BUILD)/posix_host.o: $(SHIM)/posix_host.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -I$(SHIM) -c $< -o $@

$(BUILD)/mman_win.o: $(SHIM)/mman_win.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -I$(SHIM) -c $< -o $@

# Module sources from src/kernel/nt.
$(BUILD)/%.o: $(NT)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# The driver host's service database (src/kernel/nt/driver/drvhost_svcdb.c): plain
# C over the registry API, so it builds with the same kshim flags as the modules.
$(BUILD)/drvhost_svcdb.o: $(NT)/driver/drvhost_svcdb.c $(NT)/driver/drvhost_svcdb.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(NTDLL) -c $< -o $@

# The driver host's registry bridge (src/kernel/nt/driver/drvhost_regbridge.c): the same, for the Zw and Rtl registry calls.
$(BUILD)/drvhost_regbridge.o: $(NT)/driver/drvhost_regbridge.c $(NT)/driver/drvhost_regbridge.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(NTDLL) -c $< -o $@

# The PE parser from src/loader, built plain (no kernel prelude), like tests/host.
$(BUILD)/pe_parse.o: $(LOADER)/pe_parse.c $(LOADER)/pe_parse.h $(LOADER)/pe_format.h | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -I$(LOADER) -c $< -o $@

# Real PE fixtures built by tests/host (MinGW). image_plan_test reads them as
# bytes and never runs them. Built before the checks so the fixture tests run.
HOST_FIX := ../host/out/fixtures/minimal.exe ../host/out/fixtures/exports.dll
$(HOST_FIX):
	$(MAKE) -C ../host $(patsubst ../host/%,%,$@)

# ntstatus_kernel.c includes ntstatus_table.inc, generated from the userspace
# table in src/ntdll/ntstatus_to_dos.c. gen_ntstatus_table.py is the canonical
# generator; it needs python3, which is not installed here (the Store alias
# is a stub), so the entry lines are extracted with awk/sed instead. The
# output is the same set of entry lines; see README.md.
$(NTSTATUS_INC): $(NTDLL)/ntstatus_to_dos.c tools/ntstatus_inc.sh | $(BUILD)
	sh tools/ntstatus_inc.sh $< $@

$(BUILD)/ntstatus_kernel.o: $(NTSTATUS_INC)

# io.c and nt_obj.c both define strong nt_svc_NtClose in their userspace
# branches. io_test uses io.c's file-aware NtClose, so the nt_obj.c copy is
# weakened for the link (see README.md).
$(BUILD)/nt_obj_weak.o: $(BUILD)/nt_obj.o
	objcopy --weaken-symbol=nt_svc_NtClose $< $@

# Linux scratch-root hook (shim/linux/khost_linux.c).
$(BUILD)/khost_linux.o: $(SHIM)/linux/khost_linux.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -c $< -o $@

# PE loader units from src/loader, built plain like pe_parse.o.
$(BUILD)/pe_prot.o: $(LOADER)/pe_prot.c $(LOADER)/pe_prot.h $(LOADER)/pe_parse.h $(LOADER)/pe_format.h | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -I$(LOADER) -c $< -o $@

$(BUILD)/pe_text.o: $(LOADER)/pe_text.c $(LOADER)/pe_text.h $(LOADER)/pe_prot.h | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -I$(LOADER) -c $< -o $@

# binfmt_pe_test and its userspace kernel model, both from src/kernel/fs.
$(BUILD)/fs_kshim.o: $(FS)/kshim.c $(FS)/kshim.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/binfmt_pe_test.o: $(FS)/binfmt_pe_test.c $(FS)/binfmt_pe.c $(FS)/kshim.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Shim self-test: plain host C against the shim, without the module prelude.
# On Linux it includes the glibc sys/mman.h, so it checks the real kernel's mmap.
$(BUILD)/mman_test.o: $(SHIM)/mman_test.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -D_GNU_SOURCE $(SHIM_INC) -c $< -o $@

$(BUILD)/mman_test.exe: $(BUILD)/mman_test.o $(SHIM_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Link rule: each program takes its own test object plus its module objects.
define PROG_RULE
$(BUILD)/$(1).exe: $(BUILD)/$(1).o $(addprefix $(BUILD)/,$(addsuffix .o,$($(1)_OBJS))) $(SHIM_OBJS)
	$(CC) $(LDFLAGS) -o $$@ $$^ $(LDLIBS)
endef

# Test translation units: NT/<name>.c -> build/<name>.o.
$(foreach t,$(filter-out $(LOCAL_TESTS) $(FS_TESTS),$(ALL_PROGS)),$(eval $(BUILD)/$(t).o: $(NT)/$(t).c | $(BUILD); $$(CC) $$(CFLAGS) -c $$< -o $$@))
$(foreach t,$(LOCAL_TESTS),$(eval $(BUILD)/$(t).o: $(t).c | $(BUILD); $$(CC) $$(CFLAGS) -c $$< -o $$@))
$(foreach t,$(ALL_PROGS),$(eval $(call PROG_RULE,$(t))))

# Per-program wall-clock limit in seconds. sync_test is slow on Windows
# (its wait_on_address loop sleeps 2 ms per iteration).
TIMEOUT ?= 3000

# Iterations of sync_test's wait_on_address_extended loop (SYNC_TEST_ITERATIONS).
# "check" and "check-quick" use the reduced count; "check-full" uses the CI
# count (20000, the program's default). Every program gets the variable, and
# only sync_test reads it.
SYNC_CHECK_ITERATIONS ?= 2000
SYNC_FULL_ITERATIONS  ?= 20000

# Programs run by "check-quick": all of TESTS except sync_test.
QUICK_TESTS := $(filter-out sync_test,$(TESTS))

# RUN_TESTS, LIST, ITERATIONS: run each program in LIST from $(BUILD), print
# its rc, and exit nonzero if any failed. Logs go to $(BUILD)/<name>.log.
RUN_TESTS = fail=0; for t in $(1); do \
	  (cd $(BUILD) && SYNC_TEST_ITERATIONS=$(2) timeout $(TIMEOUT) ./$$t.exe > $$t.log 2>&1); rc=$$?; \
	  echo "$$t rc=$$rc"; [ $$rc -eq 0 ] || fail=1; \
	done; exit $$fail

check: all $(HOST_FIX)
	@$(call RUN_TESTS,$(TESTS) $(SHIM_TESTS),$(SYNC_CHECK_ITERATIONS))

check-quick: all $(HOST_FIX)
	@$(call RUN_TESTS,$(QUICK_TESTS) $(SHIM_TESTS),$(SYNC_CHECK_ITERATIONS))

check-full: all $(HOST_FIX)
	@$(call RUN_TESTS,$(TESTS) $(SHIM_TESTS),$(SYNC_FULL_ITERATIONS))

# Concurrency stress (not part of check). Each STRESS_TESTS program runs once
# per seed in STRESS_SEEDS with STRESS_THREADS workers (8..16) and STRESS_OPS
# operations per worker. KSHIM_POISON=1 poisons allocations. A failure prints
# the seed, so rerun with: make stress STRESS_SEEDS=<seed>.
STRESS_SEEDS   ?= 1 2 3
STRESS_THREADS ?= 12
STRESS_OPS     ?=
STRESS_TIMEOUT ?= 900

stress: all
	@fail=0; for s in $(STRESS_SEEDS); do for t in $(STRESS_TESTS); do \
	  (cd $(BUILD) && KSHIM_POISON=1 timeout $(STRESS_TIMEOUT) ./$$t.exe $$s $(STRESS_THREADS) $(STRESS_OPS) > $$t.seed$$s.log 2>&1); rc=$$?; \
	  echo "$$t seed=$$s rc=$$rc"; [ $$rc -eq 0 ] || fail=1; \
	done; done; exit $$fail

.PHONY: stress

clean:
	rm -rf $(BUILD)

# Removes scratch roots that earlier runs left in the temp dir: directories
# named ntio_test_* (exact prefix) whose mtime is more than 1 day old, in the
# same TEMP_DIR the tests use. Nothing else in that dir is touched. Under MSYS,
# find must be the MSYS one (C:/msys64/usr/bin first on PATH, see TOOLCHAIN.md).
clean-scratch:
	@d=$$(cygpath -u "$(or $(TEMP_DIR),/tmp)" 2>/dev/null || echo "$(or $(TEMP_DIR),/tmp)"); \
	n=$$(find "$$d" -maxdepth 1 -mindepth 1 -type d -name 'ntio_test_*' -mtime +1 -print | wc -l); \
	find "$$d" -maxdepth 1 -mindepth 1 -type d -name 'ntio_test_*' -mtime +1 -exec rm -rf {} +; \
	echo "clean-scratch: removed $$n stale ntio_test_* dirs from $$d"

.PHONY: clean-scratch
