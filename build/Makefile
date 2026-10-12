# Top-level Makefile for project ntpers.
#
# Paths contain spaces, so every path reference is quoted.

PYTHON ?= python3

.PHONY: all test conformance clean validate-criteria ntstatus-table

# Repository root, derived from this Makefile's location. PROJ defaults to it;
# override PROJ only to point these targets at another tree.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PROJ ?= $(ROOT)
HARNESS := $(PROJ)/tests/harness/dodrun.py
SELFTEST := $(PROJ)/tests/harness/selftest.py
VALIDATE := $(PROJ)/build/validate_criteria.py
NTSTATUS_SRC := src/ntdll/ntstatus_to_dos.c
NTSTATUS_GEN := src/kernel/nt/gen_ntstatus_table.py
NTSTATUS_INC := src/kernel/nt/ntstatus_table.inc

# -----------------------------------------------------------------------------
# all: build userspace components produced by other workstreams.
# Tolerates absence of those directories with a clear message.
# -----------------------------------------------------------------------------
all:
	@echo "=== ntpers top-level build ==="
	@echo "Project root: $(PROJ)"
	@if [ -d "$(PROJ)/userspace" ]; then \
		echo "Building userspace components..."; \
		$(MAKE) -C "$(PROJ)/userspace" || echo "WARN: userspace build failed"; \
	else \
		echo "INFO: userspace/ not present yet (other workstreams); skipping."; \
	fi
	@if [ -d "$(PROJ)/kernel" ]; then \
		echo "INFO: kernel/ present; kernel build is not invoked by the top-level Makefile."; \
	else \
		echo "INFO: kernel/ not present yet; skipping."; \
	fi
	@if [ -x "$(PROJ)/build/build_pe_corpus.sh" ]; then \
		echo "INFO: PE corpus builder present; run it manually with mingw-w64 installed."; \
	else \
		echo "INFO: PE corpus builder not present."; \
	fi
	@echo "=== top-level build step complete ==="

# -----------------------------------------------------------------------------
# validate-criteria: sanity-check the authoritative criteria file.
# -----------------------------------------------------------------------------
validate-criteria:
	$(PYTHON) "$(VALIDATE)"

# -----------------------------------------------------------------------------
# test: run the harness self-tests (anti-gaming guard).
# -----------------------------------------------------------------------------
test:
	$(PYTHON) "$(SELFTEST)"

# -----------------------------------------------------------------------------
# conformance: run the full Definition-of-Done conformance suite.
# -----------------------------------------------------------------------------
conformance:
	$(PYTHON) "$(HARNESS)"

# -----------------------------------------------------------------------------
# ntstatus-table: regenerate the kernel NTSTATUS->DOS table
# (src/kernel/nt/ntstatus_table.inc) from the userspace source of truth.
# Runs from the repository root so the header's source path is stable.
# -----------------------------------------------------------------------------
ntstatus-table: $(ROOT)/$(NTSTATUS_INC)

$(ROOT)/$(NTSTATUS_INC): $(ROOT)/$(NTSTATUS_SRC) $(ROOT)/$(NTSTATUS_GEN)
	cd "$(ROOT)" && $(PYTHON) "$(NTSTATUS_GEN)" --input "$(NTSTATUS_SRC)" --output "$(NTSTATUS_INC)"

# -----------------------------------------------------------------------------
# clean: remove generated reports and build artifacts we own.
# -----------------------------------------------------------------------------
clean:
	rm -rf "$(PROJ)/reports"
	@echo "Cleaned $(PROJ)/reports."
