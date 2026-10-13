# Claim boundary

This document states what the project does not claim about third-party drivers and
hardware. It is checked by `tools/docs_lint.py --policy --criterion DOD-DRV-B003`,
which is registered for `DOD-DRV-B003` in `tests/harness/registry.yaml`. The claim
surface is the same as in `INTEGRITY-POLICY.md`.

## Boundary statement

No third-party NT `.sys` executes on the Personality. A third-party driver is any
NT kernel-mode driver not built and signed by this project. Production WHQL-signed
vendor drivers are gated: their owners are Microsoft and each vendor (see the
EXT-GATED register, `docs/phase0/DEFINITION-OF-DONE.md` section 3.1).

## Hardware claims

A sentence that claims hardware support must name the API subset and the device
class it covers, in the same sentence. The lint checks for the phrase "hardware
support", and requires both "API subset" and "device class" in that sentence.

## Gated claims and waivers

The criterion refers to a waiver under DoD section 11. This repository's DoD
document does not contain a section 11; sections 5 to 13 are summarized in one
line. The waiver form is therefore not defined here. Until it is, the lint treats
any affirmed third-party or gated driver claim as a violation, and no waiver path
exists.

## Cross-references

- `DOD-DRV-B003`: this document is its artifact.
- `DOD-DRV-A001`, `DOD-DRV-B005`: production WHQL-signed third-party `.sys` (gated).
- `DOD-DRV-A002`, `DOD-DRV-B004`: the project's own device model, signed by the project's test CA.
- `INTEGRITY-POLICY.md`: the kernel-integrity claims that sit alongside this boundary.
