# Top-level Makefile for project ntpers.
#
# Paths contain spaces, so every path reference is quoted.

PROJ := /path/to/ntpers-research
PYTHON := python3
HARNESS := $(PROJ)/tests/harness/dodrun.py
SELFTEST := $(PROJ)/tests/harness/selftest.py
VALIDATE := $(PROJ)/build/validate_criteria.py

.PHONY: all test conformance clean validate-criteria

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
# clean: remove generated reports and build artifacts we own.
# -----------------------------------------------------------------------------
clean:
	rm -rf "$(PROJ)/reports"
	@echo "Cleaned $(PROJ)/reports."
