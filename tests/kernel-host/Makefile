# tests/kernel-host/Makefile - build and run the src/kernel/nt unit tests on
# the Windows host (UCRT gcc) against the userspace kshim (USERSPACE_KSHIM).
# See README.md for the module-by-module status and the shim layout.
#
# Toolchain: C:/Users/Jackson/repos/TOOLCHAIN.md. Run make from a shell that
# already has /c/msys64/ucrt64/bin on PATH.

export TMP    := C:/Users/Jackson/AppData/Local/Temp
export TEMP   := C:/Users/Jackson/AppData/Local/Temp
export TMPDIR := /c/Users/Jackson/AppData/Local/Temp

CC      := gcc
NT      := ../../src/kernel/nt
NTDLL   := ../../src/ntdll
SHIM    := shim
BUILD   := build
NTSTATUS_INC := $(BUILD)/ntstatus_table.inc

WARN    := -Wall -Wextra
DEPFLAGS := -MMD -MP
CFLAGS  := -std=gnu11 -g -O0 $(WARN) $(DEPFLAGS) -DUSERSPACE_KSHIM -DKHOST_LINUX_STAT \
           -include $(SHIM)/khost_prelude.h \
           -iquote $(NT) -iquote $(BUILD) -I$(SHIM)
LDLIBS  := -lpthread -lws2_32
LDFLAGS := -fno-use-linker-plugin

# Host-only POSIX primitives (see shim/).
SHIM_OBJS := $(BUILD)/mman_win.o $(BUILD)/posix_host.o $(BUILD)/posix_raw.o

# Test programs and the module objects each one links against.
TESTS := object_test security_test registry_test i2_test io_test mm_test sync_test

# Built but not run by "check": exits 0 before printing its summary (see README).
BUILD_ONLY := proc_thread_test
ALL_PROGS := $(TESTS) $(BUILD_ONLY)

object_test_OBJS      := object handle kshim
sync_test_OBJS        := sync kshim
security_test_OBJS    := security kshim
mm_test_OBJS          := mm kshim
registry_test_OBJS    := registry object handle nt_obj sync kshim
io_test_OBJS          := io object handle iocp nt_obj_weak sync kshim
proc_thread_test_OBJS := proc_thread object handle nt_obj sync nt_services kshim
i2_test_OBJS          := object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel

PROGS := $(addprefix $(BUILD)/,$(addsuffix .exe,$(ALL_PROGS)))

.PHONY: all check clean
all: $(PROGS)

$(BUILD):
	mkdir -p $(BUILD)

-include $(wildcard $(BUILD)/*.d)

# Host shim units. These are built without the prelude and, for the POSIX
# wrappers, with only the shim include directory.
$(BUILD)/posix_raw.o: $(SHIM)/posix_raw.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) -c $< -o $@

$(BUILD)/posix_host.o: $(SHIM)/posix_host.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) -I$(SHIM) -c $< -o $@

$(BUILD)/mman_win.o: $(SHIM)/mman_win.c | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) $(DEPFLAGS) -I$(SHIM) -c $< -o $@

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

# Link rule: each program takes its own test object plus its module objects.
define PROG_RULE
$(BUILD)/$(1).exe: $(BUILD)/$(1).o $(addprefix $(BUILD)/,$(addsuffix .o,$($(1)_OBJS))) $(SHIM_OBJS)
	$(CC) $(LDFLAGS) -o $$@ $$^ $(LDLIBS)
endef

# Test translation units: NT/<name>.c -> build/<name>.o.
$(foreach t,$(ALL_PROGS),$(eval $(BUILD)/$(t).o: $(NT)/$(t).c | $(BUILD); $$(CC) $$(CFLAGS) -c $$< -o $$@))
$(foreach t,$(ALL_PROGS),$(eval $(call PROG_RULE,$(t))))

# Per-program wall-clock limit in seconds. sync_test is slow on Windows
# (its wait_on_address loop sleeps 2 ms per iteration).
TIMEOUT ?= 3000

check: all
	@fail=0; for t in $(TESTS); do \
	  (cd $(BUILD) && timeout $(TIMEOUT) ./$$t.exe > $$t.log 2>&1); rc=$$?; \
	  echo "$$t rc=$$rc"; [ $$rc -eq 0 ] || fail=1; \
	done; exit $$fail

clean:
	rm -rf $(BUILD)
