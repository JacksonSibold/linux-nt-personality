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

CC      := gcc
NT      := ../../src/kernel/nt
NTDLL   := ../../src/ntdll
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
CFLAGS  := -std=gnu11 -g $(OPT) $(WARN) $(DEPFLAGS) $(COV_FLAGS) -DUSERSPACE_KSHIM \
           -include $(SHIM)/khost_prelude.h \
           -iquote $(NT) -iquote $(BUILD) -I$(SHIM)
LDLIBS  := -lpthread -lws2_32 $(COV_LIBS)
LDFLAGS := -fno-use-linker-plugin $(COV_FLAGS)

# Host-only POSIX primitives (see shim/).
SHIM_OBJS := $(BUILD)/mman_win.o $(BUILD)/posix_host.o $(BUILD)/posix_raw.o

# Test programs and the module objects each one links against.
TESTS := security_extra_test object_test security_test registry_test i2_test io_test mm_test sync_test sync_extra_test proc_thread_test handle_edge_test registry_ops_test

# Tests kept in this directory (not in src/kernel/nt). Their sources are $(t).c here.
LOCAL_TESTS := handle_edge_test registry_ops_test

# Concurrency stress programs (see README.md "Stress tests"). Run by "stress",
# never by "check". Sources are in this directory, like LOCAL_TESTS.
STRESS_TESTS := stress_handle_test stress_object_test stress_sync_test
LOCAL_TESTS += $(STRESS_TESTS)

# Built but not run by "check" (none at present).
BUILD_ONLY :=
ALL_PROGS := $(TESTS) $(BUILD_ONLY) $(STRESS_TESTS)

object_test_OBJS      := security object handle kshim
sync_test_OBJS        := security sync kshim object handle nt_obj
sync_extra_test_OBJS  := security object handle nt_obj sync kshim
security_extra_test_OBJS      := security object handle kshim
security_test_OBJS    := security object handle kshim
mm_test_OBJS          := mm kshim
registry_test_OBJS    := security registry object handle nt_obj sync kshim
io_test_OBJS          := security io object handle iocp nt_obj_weak sync kshim
proc_thread_test_OBJS := security proc_thread object handle nt_obj sync nt_services kshim
i2_test_OBJS          := security object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel
handle_edge_test_OBJS := security object handle kshim
registry_ops_test_OBJS := security registry object handle nt_obj sync kshim
stress_handle_test_OBJS := security object handle kshim
stress_object_test_OBJS := security object handle kshim
stress_sync_test_OBJS := security object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel

# Shim self-tests (shim/mman_test.c). Run by "check"; not kernel module tests.
SHIM_TESTS := mman_test

PROGS := $(addprefix $(BUILD)/,$(addsuffix .exe,$(ALL_PROGS) $(SHIM_TESTS)))

.PHONY: all check clean
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

# Shim self-test: plain host C against the shim, without the module prelude.
$(BUILD)/mman_test.o: $(SHIM)/mman_test.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) $(COV_FLAGS) -I$(SHIM) -c $< -o $@

$(BUILD)/mman_test.exe: $(BUILD)/mman_test.o $(SHIM_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Link rule: each program takes its own test object plus its module objects.
define PROG_RULE
$(BUILD)/$(1).exe: $(BUILD)/$(1).o $(addprefix $(BUILD)/,$(addsuffix .o,$($(1)_OBJS))) $(SHIM_OBJS)
	$(CC) $(LDFLAGS) -o $$@ $$^ $(LDLIBS)
endef

# Test translation units: NT/<name>.c -> build/<name>.o.
$(foreach t,$(filter-out $(LOCAL_TESTS),$(ALL_PROGS)),$(eval $(BUILD)/$(t).o: $(NT)/$(t).c | $(BUILD); $$(CC) $$(CFLAGS) -c $$< -o $$@))
$(foreach t,$(LOCAL_TESTS),$(eval $(BUILD)/$(t).o: $(t).c | $(BUILD); $$(CC) $$(CFLAGS) -c $$< -o $$@))
$(foreach t,$(ALL_PROGS),$(eval $(call PROG_RULE,$(t))))

# Per-program wall-clock limit in seconds. sync_test is slow on Windows
# (its wait_on_address loop sleeps 2 ms per iteration).
TIMEOUT ?= 3000

check: all
	@fail=0; for t in $(TESTS) $(SHIM_TESTS); do \
	  (cd $(BUILD) && timeout $(TIMEOUT) ./$$t.exe > $$t.log 2>&1); rc=$$?; \
	  echo "$$t rc=$$rc"; [ $$rc -eq 0 ] || fail=1; \
	done; exit $$fail

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
