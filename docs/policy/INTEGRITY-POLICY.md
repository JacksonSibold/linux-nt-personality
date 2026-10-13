# Integrity policy

This policy governs what the public documentation may say about operating-system
integrity. It is checked by `tools/docs_lint.py --policy --criterion DOD-SEC-A008`,
which is registered for `DOD-SEC-A008` in `tests/harness/registry.yaml`.

## Scope

The claim surface is every Markdown file in the repository root, plus
`docs/status/*.md` and `docs/policy/*.md`. Design notes under `docs/phase1/` and
`docs/phase2/` are analysis, not claims, and are not scanned.

## Claims

The Personality makes no claim to Windows-update-signed kernel properties.
No statement in the claim surface asserts that the kernel image is Microsoft-signed,
that it was delivered through Windows Update, or that a Microsoft signing chain
verifies it at load time.

## Out-of-scope mechanisms

- PatchGuard (Kernel Patch Protection) is out of scope. It is not implemented and is
  not claimed. Gated criteria that depend on it: `DOD-AC-A001`, `DOD-AC-B003`.
- Secure Kernel and Virtualization-Based Security, including HVCI, are out of scope.
  They are not implemented and are not claimed. Gated criteria: `DOD-AC-A001`, `DOD-AC-B003`.
- Windows-update-signed kernel images and Microsoft-signed kernel binaries are out of
  scope. Their owner is Microsoft; see the EXT-GATED register in
  `docs/phase0/DEFINITION-OF-DONE.md` section 3.1.

## Rules for public text

1. A sentence that names PatchGuard, the Secure Kernel, VBS, HVCI, or a
   Windows-update-signed or Microsoft-signed kernel must say that the mechanism is
   out of scope, not implemented, gated, or not claimed.
2. A sentence may not affirm support for any of those mechanisms.
3. Every criterion ID cited in this policy must exist in `tests/conformance/criteria.json`.

## Cross-references

- `DOD-SEC-A008`: this policy is its artifact.
- `DOD-AC-A005`, `DOD-AC-B001`: OS-identity integrity. A version claim must match
  the declared compatibility policy; this document does not change that.
- `DOD-AC-A001`, `DOD-AC-B003`: kernel anti-cheat, which depends on PatchGuard and HVCI.
- `DOD-DRV-B003`: the claim boundary for third-party drivers, in `CLAIM-BOUNDARY.md`.
