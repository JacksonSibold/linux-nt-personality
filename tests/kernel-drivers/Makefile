# tests/kernel-drivers - NT kernel-mode driver host: unit tests and the hello_drv
# end-to-end test.  Runs in the Linux guest (gcc + MinGW), not on the Windows host.
#
#   make check     build the host, the test drivers, both test programs; run them
#   make surface   regenerate docs/status/DRIVERHOST-EXPORTS.md (needs python3)
#   make clean
#
# The test .sys images are built with x86_64-w64-mingw32-gcc and linked against
# import libraries generated from the host's own export table (gen_def.sh), so a
# driver's imports cannot name a function the host does not implement by accident.

CC       ?= gcc
MINGW    ?= x86_64-w64-mingw32-gcc
DLLTOOL  ?= x86_64-w64-mingw32-dlltool
PY       ?= python3

DRV    := ../../src/kernel/nt/driver
LOADER := ../../src/loader
NTDLL  := ../../src/ntdll
BUILD  := build
SYSROOT := $(BUILD)/sysroot

WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS := -std=gnu11 -g -O1 $(WARN) -I$(DRV) -I$(LOADER) -I$(NTDLL) -I.
LDLIBS := -lpthread

HOST_SRCS := drvhost_plat drvhost_ex drvhost_sync drvhost_mm drvhost_io drvhost_ioext drvhost_pnp drvhost_rtl drvhost_fmt \
             drvhost_pe drvhost_svc drvhost_nt drvhost_exports
HOST_OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(HOST_SRCS))) $(BUILD)/pe_parse.o

# The personality registry (service records) and the object manager, built by the
# kernel-host harness with its kshim flags, plus its Linux shim.  The driver host
# reads service records through drvhost_svcdb.o, so the test links these.
NT := ../../src/kernel/nt
KH := ../kernel-host
KH_BUILD := $(KH)/build
KH_OBJS := $(addprefix $(KH_BUILD)/,$(addsuffix .o,security registry object handle nt_obj sync kshim drvhost_svcdb khost_linux))
$(KH_OBJS):
	$(MAKE) -C $(KH) $(patsubst $(KH)/%,%,$@)
HOST_OBJS += $(KH_OBJS)

.PHONY: all check surface clean layout

# Layout check: the size and the field offsets of the structures that drivers read and write directly (layout/layout_list.h),
# compared between Wine's WDK headers (third_party/wine/include, which follow the Windows definitions) and the host's types.
WINE_INC := ../../third_party/wine/include

$(BUILD)/layout_wine: layout/layout_wine.c layout/layout_list.h | $(BUILD)
	$(CC) -std=gnu11 -DWINE_UNIX_LIB -D__WINESRC__ -I$(WINE_INC) -Ilayout $< -o $@

$(BUILD)/layout_host: layout/layout_host.c layout/layout_list.h $(DRV)/drvhost_types.h | $(BUILD)
	$(CC) -std=gnu11 -I$(DRV) -I$(LOADER) -I$(NTDLL) -Ilayout $< -o $@

layout: $(BUILD)/layout_wine $(BUILD)/layout_host
	$(BUILD)/layout_wine > $(BUILD)/layout_wine.txt
	$(BUILD)/layout_host > $(BUILD)/layout_host.txt
	diff -u $(BUILD)/layout_wine.txt $(BUILD)/layout_host.txt
	@echo "layout: $$(wc -l < $(BUILD)/layout_wine.txt) measurements match the WDK definitions"

all: $(BUILD)/driver_unit_test $(BUILD)/driver_sync_test $(BUILD)/driver_mm_test $(BUILD)/driver_ioext_test $(BUILD)/driver_host_test \
     $(SYSROOT)/drivers/hello_drv.sys $(SYSROOT)/drivers/bad_import.sys

$(BUILD):
	mkdir -p $(BUILD) $(SYSROOT)/drivers

# Host objects: the driver host itself, compiled with the Microsoft x64 ABI
# attribute on every driver-facing function (drvhost_types.h, DRVCALL).
$(BUILD)/%.o: $(DRV)/%.c $(DRV)/drvhost_types.h $(DRV)/drvhost_ntos.h $(DRV)/drvhost_int.h $(DRV)/drvhost.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# The PE parser from src/loader, built plain (no kernel prelude).
$(BUILD)/pe_parse.o: $(LOADER)/pe_parse.c $(LOADER)/pe_parse.h $(LOADER)/pe_format.h | $(BUILD)
	$(CC) -std=gnu11 -g $(WARN) -I$(LOADER) -c $< -o $@

$(BUILD)/driver_unit_test.o: driver_unit_test.c dh_test.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/driver_host_test.o: driver_host_test.c dh_test.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/driver_unit_test: $(BUILD)/driver_unit_test.o $(HOST_OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/driver_sync_test.o: driver_sync_test.c dh_test.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/driver_sync_test: $(BUILD)/driver_sync_test.o $(HOST_OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/driver_mm_test.o: driver_mm_test.c dh_test.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/driver_mm_test: $(BUILD)/driver_mm_test.o $(HOST_OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/driver_ioext_test.o: driver_ioext_test.c dh_test.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/driver_ioext_test: $(BUILD)/driver_ioext_test.o $(HOST_OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/reg_boot.o: reg_boot.c | $(BUILD)
	$(CC) -std=gnu11 -g -O1 -DUSERSPACE_KSHIM -include $(KH)/shim/linux/khost_prelude.h \
	    -I$(KH)/shim/linux -iquote $(NT) -iquote $(NTDLL) -c $< -o $@

$(BUILD)/driver_host_test: $(BUILD)/driver_host_test.o $(BUILD)/reg_boot.o $(HOST_OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

# Import libraries from the host's own export table.
$(BUILD)/ntoskrnl.def: $(DRV)/drvhost_exports.c gen_def.sh | $(BUILD)
	sh gen_def.sh $(DRV)/drvhost_exports.c ntos > $@

$(BUILD)/hal.def: $(DRV)/drvhost_exports.c gen_def.sh | $(BUILD)
	sh gen_def.sh $(DRV)/drvhost_exports.c hal > $@

# Deliberately absent from the host: the bad-import fixture links against it.
$(BUILD)/badimp.def: | $(BUILD)
	printf 'LIBRARY ntoskrnl.exe\nEXPORTS\nNoSuchFunctionXyz\n' > $@

$(BUILD)/libntoskrnl.a: $(BUILD)/ntoskrnl.def
	$(DLLTOOL) -d $< -l $@

$(BUILD)/libhal.a: $(BUILD)/hal.def
	$(DLLTOOL) -d $< -l $@

$(BUILD)/libbadimp.a: $(BUILD)/badimp.def
	$(DLLTOOL) -d $< -l $@

# Native (subsystem 1) x64 PE images, DriverEntry as the entry point.
MINGW_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-stack-protector -I$(DRV) -DDRVHOST_DRIVER_BUILD
MINGW_LDFLAGS := -nostdlib -Wl,--subsystem,native -Wl,--entry,DriverEntry -Wl,--file-alignment,0x200 -Wl,--disable-auto-import

$(BUILD)/hello_drv.o: hello_drv/hello_drv.c $(DRV)/drvhost_types.h $(DRV)/drvhost_ntos.h | $(BUILD)
	$(MINGW) $(MINGW_CFLAGS) -c $< -o $@

$(SYSROOT)/drivers:
	mkdir -p $@

$(SYSROOT)/drivers/hello_drv.sys: $(BUILD)/hello_drv.o $(BUILD)/libntoskrnl.a $(BUILD)/libhal.a | $(BUILD) $(SYSROOT)/drivers
	$(MINGW) $(MINGW_LDFLAGS) -o $@ $< -L$(BUILD) -lntoskrnl -lhal -lgcc

$(BUILD)/bad_import.o: bad_import_drv/bad_import.c $(DRV)/drvhost_types.h $(DRV)/drvhost_ntos.h | $(BUILD)
	$(MINGW) $(MINGW_CFLAGS) -c $< -o $@

$(SYSROOT)/drivers/bad_import.sys: $(BUILD)/bad_import.o $(BUILD)/libbadimp.a | $(BUILD) $(SYSROOT)/drivers
	$(MINGW) $(MINGW_LDFLAGS) -o $@ $< -L$(BUILD) -lbadimp -lgcc

check: all layout
	$(BUILD)/driver_unit_test
	$(BUILD)/driver_sync_test
	$(BUILD)/driver_mm_test
	$(BUILD)/driver_ioext_test
	$(BUILD)/driver_host_test $(SYSROOT)

surface:
	$(PY) ../../tools/gen_ntos_surface.py --repo ../.. --out ../../docs/status/DRIVERHOST-EXPORTS.md

clean:
	rm -rf $(BUILD)
