# Claim boundary: third-party drivers

This document is the claim boundary for third-party driver code. It is checked by
`tools/docs_lint.py --policy --criterion DOD-DRV-B003`, which is the artifact of
`DOD-DRV-B003`. The integrity rules for kernel mechanisms are in `INTEGRITY-POLICY.md`.

## Statement

No third-party NT `.sys` executes in the Personality. The project does not load, run,
or ship a vendor driver that carries a production signature from a third party.

## Basis

The statement rests on the absence of a driver-load path in the tracked tree. A
`git grep` for `NtLoadDriver`, `ZwLoadDriver`, `IoCreateDriver` and `DriverEntry`
over `src/`, `arch/`, `build/` and `tests/` finds no match, and `git ls-files` lists
no `.sys` file. This is evidence about the source snapshot, not a test result: no
test loads a driver, and no harness run covers this statement.

## Gated driver criteria

These criteria need a third-party artifact the project cannot produce. They are not
implemented and are not claimed:

- `DOD-DRV-A001` and `DOD-DRV-B005` concern a production-signed vendor `.sys`. Owner:
  Microsoft signing infrastructure and each vendor. See the EXT-GATED register in
  `docs/phase0/DEFINITION-OF-DONE.md` section 3.1.

A public statement that a gated driver runs needs a waiver under section 11 of the
definition of done. Without one, the statement is a violation. The repository has no
section 11 of its own: `docs/phase0/DEFINITION-OF-DONE.md` incorporates sections 5 to 13
by reference from documents outside this repository. The repository records no waiver.

## Device support wording

Any statement about device support must name the API subset and the device class it
covers. A statement that names neither is a violation of `DOD-DRV-B003`.
