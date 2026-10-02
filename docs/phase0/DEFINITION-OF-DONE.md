# Authoritative Definition of Done

**Status:** RATIFIED (Phase 0 complete)
**Machine-readable criteria set:** `tests/conformance/criteria.json` (423 criteria)
**Sources reconciled:** `dod-agent-a.md` (214 unique), `dod-agent-b.md` (209 unique)

---

## §1 Scope and authority

This document and `criteria.json` are the **sole authority** on project completion.

Agent claims are not authority. Research documents are not authority. Architecture
documents are not authority. The executable test suite decides.

The system under test (SUT) is an Ubuntu 26.04 LTS-derived operating system in which
Windows PE executables run as a **first-class native execution personality** alongside
ELF/Linux, sharing kernel infrastructure but *not* routed through a user-mode
Win32→POSIX compatibility server.

**Non-negotiables carried into every criterion:** no Wine/Proton lineage; no CPU
emulation or binary translation on x86-64; no VM in the execution path; Windows
machine code executes directly on the host CPU; no Linux regression.

---

## §2 Tiering

| Tier | Count | Meaning | Release gate |
|---|---|---|---|
| **P0** | 143 | Project is meaningless without it | 100% pass required |
| **P1** | 175 | Core experience | ≥98% pass; every failure has a filed defect |
| **P2** | 89 | Breadth | ≥85% pass; gaps must return correct error codes |
| **P3** | 16 | Long tail | Best effort; must be declared in the capability matrix |

---

## §3 Dependency classes

| Class | Count | Meaning |
|---|---|---|
| `SELF` | 131 | Achievable purely by our own engineering |
| `EXT-DOC` | 227 | Needs large but public documentation/specs |
| `EXT-RE` | 48 | Needs reverse engineering of undocumented behaviour |
| `EXT-GATED` | 17 | Needs an artifact controlled by a third party |

### §3.1 EXT-GATED register (governance)

Both Definition-of-Done authors worked independently and **converged on the same
gated set**. This register exists so that these criteria can never be silently
reclassified as passed. A gated criterion may be *waived with evidence*, but it may
never be *reported as passing* unless the controlling artifact was actually obtained.

| Criteria | Artifact required | Controlling party |
|---|---|---|
| `DOD-AC-A001`, `DOD-AC-B003` | Kernel-mode AC (Vanguard/BattlEye/EAC) attestation of a genuine-NT kernel, PatchGuard, HVCI, measured Secure Boot chain | Riot / BattlEye / Epic + Microsoft |
| `DOD-AC-A002` | Live VAC-protected server admission | Valve |
| `DOD-DRV-A001`, `DOD-DRV-B005` | Production WHQL-signed third-party `.sys` + vendor builds for a non-NT kernel | Microsoft signing infra + each vendor |
| `DOD-BOOTPH-A001` | Microsoft UEFI CA-signed shim for factory-default Secure Boot (MS PK/KEK) | Microsoft |
| `DOD-DRM-A002`, `DOD-DRM-B003` | Denuvo activation | Irdeto |
| `DOD-DRM-A001`, `DOD-DRM-B004` | PlayReady hardware DRM keys | Microsoft |
| `DOD-DRM-B001`, `DOD-DRM-A004` | Steam live login/store | Valve |
| `DOD-EXE-A001`, `DOD-EXE-B007` | Office LTSC / Edge / WebView2 licensed for Windows only | Microsoft |
| `DOD-UWP-A001` | Store identity + licensing infrastructure | Microsoft |
| `DOD-GFX-A001`, `DOD-GFX-B007` | Microsoft-licensed TrueType fonts for glyph-exact parity | Microsoft |
| `DOD-SEC-B010` | Ubuntu's existing MS-signed shim chain interaction | Canonical / Microsoft |

**Engineering response (not a waiver):** for each gated criterion the project MUST
still implement and test the *ungated substitute* — our own test-signing CA and
driver stack (`DOD-DRV-001..004`), custom-mode Secure Boot with a project PK
(`DOD-BOOTPH-003`), MOK enrolment, user-mode anti-tamper titles (`DOD-AC-003`),
DRM-free commercial corpora, and metric-compatible libre fonts. Progress is measured
against those.

---

## §4 Criteria table

Held in `tests/conformance/criteria.json`. Each entry carries `id`, `domain`, `tier`,
`dependency_class`, `criterion`, `verification`, `threshold`, `source`, `origin_ids`,
`dup_group`, `status`.

**Merge policy:** union-preserving. Semantic deduplication across the two documents is
not mechanically decidable; fusing two distinct criteria would corrupt both the
criterion text and the gated register, whereas retaining a genuine duplicate is
harmless. Entries are grouped by `dup_group` (domain) so true duplicates can be
collapsed during test authoring with human judgement. Where the two authors classified
the same property differently, the **worst** dependency class and **higher** tier win.

Domains (33): ARCH, API, EXE, PE, PROC, MEM, FS, REG, DLL, NET, SEC, GFX, DX, AUD,
INP, SVC, COM, UWP, DOTNET, DRV, AC, DRM, SYNC, IPC, NT, PERF, PORT, BOOTVM, BOOTPH,
LINUXR, EVI, TELEM, GDK.

---

## §5–§13 Supporting specifications

The detailed harness architecture, anti-gaming protocol, semantic-depth trap
catalogue, counterfeit-architecture detection, differential-testing methodology,
performance baselines, boot/portability procedure, Linux regression battery and
evidence model are retained in full in the two source documents and are incorporated
here by reference:

- Harness, corpus, perf methodology, counterfeit detection, hardware matrix →
  `dod-agent-a.md` §5–§9
- Falsification protocol, differential testing, 30 semantic-depth traps, staged boot
  acceptance, Linux regression battery, signed-evidence model →
  `dod-agent-b.md` §5–§13

Both are binding.

---

## §14 Key structural requirements carried forward

1. **ARCH:** ≤1 personality syscall per PE launch; ≤0.05 host syscalls per NT call;
   killing every user-mode helper immediately before launch must not prevent 100/100
   PE launches. This is the load-bearing test that distinguishes a kernel personality
   from a user-mode compatibility server.
2. **ARCH:** PMU fingerprint (retired instructions, IPC, branch-miss profile) of a PE
   binary within [0.8, 1.25]× of its same-source ELF twin; CPUID hypervisor bit == 0
   on physical runs; self-modifying-code probe behaves per Windows semantics.
3. **Provenance:** zero Wine/QEMU/Box64/FEX lineage in the image or build graph,
   proven by signed SBOM + symbol-fingerprint classifier.
4. **LINUXR:** stock-Ubuntu witness image comparison; any divergence is a failure.
5. **Evidence:** every criterion result must produce an auditable artifact bundle.
