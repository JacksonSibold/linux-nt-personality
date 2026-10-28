# Host tests for the Rtl sources ported from Wine (src/ntdll/rtl), run against Wine's own unit tests.
#
#   make -C tests/rtl-host test          build and run every suite
#   make -C tests/rtl-host out/rtlstr    build one suite
#   make -C tests/rtl-host SANITIZE=0    build without AddressSanitizer and UBSan
#
# Each suite is a file from Wine's dlls/ntdll/tests (wine/<name>.c) linked against the Rtl objects, host_support.c and a
# proctab.c that gen_proctab.py writes from the objects, so the test's GetProcAddress finds the functions. See
# docs/status/RTL-PORT.md.

ROOT := ../..
RTL := $(ROOT)/src/ntdll/rtl
WINE_INC := $(ROOT)/third_party/wine/include
OUT := out
CC ?= gcc
PYTHON ?= python3

SANITIZE ?= auto
ifeq ($(SANITIZE),auto)
  SAN := $(shell mkdir -p $(OUT) && echo 'int main(void){return 0;}' > $(OUT)/.probe.c && \
           $(CC) -fsanitize=address,undefined $(OUT)/.probe.c -o $(OUT)/.probe 2>/dev/null && $(OUT)/.probe 2>/dev/null && echo -fsanitize=address,undefined)
else ifeq ($(SANITIZE),0)
  SAN :=
else
  SAN := -fsanitize=address,undefined
endif

# -fshort-wchar: Wine's sources write L"..." strings for 16-bit WCHAR. -fno-builtin keeps the compiler from turning the
# copies of the C library functions defined in string.c into calls to themselves.
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-function -Wno-format \
          -Wno-missing-field-initializers -fshort-wchar -fno-builtin -fno-strict-aliasing $(SAN) -fno-sanitize=shift-base \
          -D__WINESRC__ -DWINE_UNIX_LIB -D_NTSYSTEM_ \
          -I$(WINE_INC) -I$(RTL)/compat -I$(RTL) -Iinclude

RTL_SRCS := rtlstr wcstring string printf large_int rtlbitmap time nls misc
RTL_OBJS := $(addprefix $(OUT)/rtl_,$(addsuffix .o,$(RTL_SRCS)))

SUITES := rtlstr large_int rtlbitmap string time

.PHONY: all test clean
all: $(addprefix $(OUT)/,$(SUITES))

test: all
	@set -e; for s in $(SUITES); do echo "== $$s"; $(OUT)/$$s; done; echo "rtl-host: all suites passed"

$(OUT):
	mkdir -p $(OUT)

$(OUT)/rtl_%.o: $(RTL)/%.c Makefile $(wildcard $(RTL)/compat/*.h) $(wildcard $(RTL)/compat/*/*.h) $(RTL)/nls_tables.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

# RtlNtStatusToDosError is the repository's own table, built with the repository's own headers.
$(OUT)/ntstatus_to_dos.o: $(ROOT)/src/ntdll/ntstatus_to_dos.c | $(OUT)
	$(CC) -std=gnu11 -O1 -g $(SAN) -I$(ROOT)/src/ntdll -c $< -o $@

$(OUT)/host_support.o: host_support.c include/wine/test.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@ -DRTL_TEST_FUNC=func_dummy -DRTL_TEST_NAME='"dummy"'

$(OUT)/proctab.c: gen_proctab.py $(RTL_OBJS) $(OUT)/host_support.o
	$(PYTHON) gen_proctab.py $@ $(RTL_OBJS) $(OUT)/host_support.o

$(OUT)/proctab.o: $(OUT)/proctab.c
	$(CC) -c $< -o $@

# One build of host_support.o per suite, since main() names the suite's entry point.
$(OUT)/support_%.o: host_support.c include/wine/test.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@ -DRTL_TEST_FUNC=func_$* -DRTL_TEST_NAME='"$*"'

# The tests call the C library with 16-bit wide strings, so they get the renamed copies from string.c, wcstring.c and
# printf.c (ntdll_wcslen, ...). swprintf is the counted _snwprintf of printf.c.
$(OUT)/test_%.o: wine/%.c include/wine/test.h Makefile | $(OUT)
	$(CC) $(CFLAGS) -include host_prelude.h -Dswprintf=ntdll__snwprintf -c $< -o $@

$(OUT)/%: $(OUT)/test_%.o $(OUT)/support_%.o $(RTL_OBJS) $(OUT)/proctab.o $(OUT)/ntstatus_to_dos.o
	$(CC) $(SAN) $^ -o $@ -lm

clean:
	rm -rf $(OUT)
