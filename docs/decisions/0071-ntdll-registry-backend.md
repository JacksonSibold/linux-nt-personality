# 0071: Registry functions always report "not found" (ntdll ADV-1)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` ADV-1 (Low, needs owner).
Date: 2026-10-12

## Context

`src/ntdll/advapi32.c:21` onward: `RegOpenKeyExA/W` and `RegQueryValueExA/W` return `ERROR_FILE_NOT_FOUND`. Installers that check for an existing key then misbehave.

## Options

1. Back the calls with the object manager registry (`src/kernel/nt/registry.c` in the kernel tree). Tradeoff: a design choice about the registry back end.
2. Keep the not-found result and document the limit. Tradeoff: software that reads the registry does not work.

## Recommendation (proposal)

Option 2 for now, stated in the compatibility notes. Option 1 when the registry is in scope.

## Patch sketch

- Option 2: document the limit only.
- Option 1: route the open and query calls to the registry module.

## Test plan

- Option 2: documentation check only.
- Option 1: open and query a key that was created through the registry module.

## Not verified

Not built or run.

## Owner question

Is a registry back end in scope for the personality, or is "not found" the documented behaviour?
