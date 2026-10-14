# Roadmap

Status date: 2026-10-13. This page says what is done, what is in progress and
what is next. Every number here is taken from a repository document, and the
source is named next to it. Where a document is older than the others, the
row says so.

Work is tracked as GitHub issues (#1 to #113 at the time of writing). The
project board is [ntpers: road to 423/423](https://github.com/users/JacksonSibold/projects/11).
Milestones are listed below. To pick up work, read [CONTRIBUTING.md](../CONTRIBUTING.md#how-to-pick-an-issue).

## Goals

The end goals are stated in [GOAL.md](GOAL.md). In short: play Rainbow Six Siege
offline on Linux from the owner's local install, and run the vendor's NVIDIA
Windows kernel-mode display driver on the personality as the GPU path. Both are
early stage. Neither is reached, and none of the tables below measures them
directly. The import gap tool ([tools/README.md](../tools/README.md)) is the
measure for the Win32 and driver-import layers. Its output for an owner's local
install is written on the owner's machine and is not part of this roadmap.

## Where things stand

| Measure | Value | Source |
|---|---|---|
| Criteria in the conformance set | 423 | [status/CRITERIA.md](status/CRITERIA.md) |
| Criteria with a dodrun PASS recorded | 3: DOD-SEC-A008, DOD-DRV-B003, DOD-EVI-B006 (bundles are unsigned) | [status/CRITERIA-PROGRESS.md](status/CRITERIA-PROGRESS.md), "dodrun verdicts" |
| Criteria unknown or not assessed | 420 | [status/CRITERIA.md](status/CRITERIA.md) |
| Criteria placed in milestones M-A to M-F | 404; 19 need a gated or out-of-scope component | [status/CRITERIA-ROADMAP.md](status/CRITERIA-ROADMAP.md), summary |
| VM differential probes, latest run (run 10) | 8 of 8 run to exit 0; 1 of 8 matches its golden at verdict level (probe_sync_direct, 74 of 74); 7 mismatch | [status/DIFFERENTIAL-RESULTS.md](status/DIFFERENTIAL-RESULTS.md) |
| Non-VM differential probes, latest cycle | 15 of 15 run to the end marker; 1 matches at verdict level (probe_sharemode, 458857 of 458857) | [status/DIFFERENTIAL-RESULTS.md](status/DIFFERENTIAL-RESULTS.md), startup-modules cycle |
| Review findings in the five reports | 221: 114 fixed, 11 partly, 41 blocked, 7 no change, 48 open (index dated 2026-10-12) | [review/INDEX.md](review/INDEX.md) |
| Decision records | 73: 15 fixed, 16 open, 41 blocked, 1 no change (snapshot 2026-10-13) | [decisions/INDEX.md](decisions/INDEX.md) |
| Owner decisions still open | 44 (OD-45 was resolved on 2026-10-13) | [OWNER-DECISIONS.md](OWNER-DECISIONS.md) |
| Services not implemented in the kernel build | 7 of 171: 5 stubs and 2 missing entries | [status/SERVICES.md](status/SERVICES.md) |

The 2026-10-12 review and the 2026-10-13 decision index were written at
different times, so the review counts may not include later fixes.

## Done

- The personality kernel boots and runs the eight VM probes to exit 0 (latest run 10).
- All 15 non-VM probes run to their end marker, after the startup-module and TEB fixes.
- The golden corpus accepts a list of Windows builds instead of one pin ([decision 0073](decisions/0073-golden-accepted-builds.md)).
- Three SELF criteria record a dodrun PASS: DOD-SEC-A008, DOD-DRV-B003 and DOD-EVI-B006.
- 114 review findings are fixed, and the fix status of each is recorded in its report.

## In progress

- Differential divergences: 21 probes have open mismatches, one issue each
  (see the `differential` label). The causes recorded so far are missing
  services, the NT namespace (the `/tmp` mount in `db518ef` addressed the
  missing `C:\tmp`), and VAD coalescing. Several causes are hypotheses in the
  document.
- Owner decisions for the P1 security and data-integrity group, OD-01 to OD-11.
- The harness integrity gate: four goldens hard-code a build that the gate rejects, so dodrun does not run ([#100](https://github.com/JacksonSibold/linux-nt-personality/issues/100)).

## Next

In dependency order, from the criteria roadmap ([status/CRITERIA-ROADMAP.md](status/CRITERIA-ROADMAP.md), section 3):

1. M-A: make dodrun run on this host ([#100](https://github.com/JacksonSibold/linux-nt-personality/issues/100)), decide the golden publication policy ([#98](https://github.com/JacksonSibold/linux-nt-personality/issues/98)), and name the external inputs: signing key ([#101](https://github.com/JacksonSibold/linux-nt-personality/issues/101)), time-stamp authority ([#102](https://github.com/JacksonSibold/linux-nt-personality/issues/102)), audit board ([#103](https://github.com/JacksonSibold/linux-nt-personality/issues/103)) and the MS-ERREF source ([#104](https://github.com/JacksonSibold/linux-nt-personality/issues/104)).
2. M-A: the owner decisions OD-01 to OD-11 (security and data integrity), starting with [OD-01](https://github.com/JacksonSibold/linux-nt-personality/issues/1).
3. M-A: NTSTATUS to Win32 table completeness, starting with [#53](https://github.com/JacksonSibold/linux-nt-personality/issues/53).
4. M-B: loader and kernel foundations, the largest set ([#107](https://github.com/JacksonSibold/linux-nt-personality/issues/107)).

## Milestones

| Milestone | Scope | Tracking issue |
|---|---|---|
| [M-A: repository-local work](https://github.com/JacksonSibold/linux-nt-personality/milestone/1) | 5 criteria; owner decisions | [#106](https://github.com/JacksonSibold/linux-nt-personality/issues/106) |
| [M-B: loader and kernel foundations](https://github.com/JacksonSibold/linux-nt-personality/milestone/2) | 20 criteria | [#107](https://github.com/JacksonSibold/linux-nt-personality/issues/107) |
| [M-C: core loader and kernel objects](https://github.com/JacksonSibold/linux-nt-personality/milestone/3) | 103 criteria | [#108](https://github.com/JacksonSibold/linux-nt-personality/issues/108) |
| [M-D: process creation, filesystem, sync, pipes, windowing base](https://github.com/JacksonSibold/linux-nt-personality/milestone/4) | 145 criteria | [#109](https://github.com/JacksonSibold/linux-nt-personality/issues/109) |
| [M-E: services, COM, networking, graphics, input, audio, CLR, WOW64](https://github.com/JacksonSibold/linux-nt-personality/milestone/5) | 110 criteria | [#110](https://github.com/JacksonSibold/linux-nt-personality/issues/110) |
| [M-F: WinRT, hardware, boot and installer chain](https://github.com/JacksonSibold/linux-nt-personality/milestone/6) | 21 criteria | [#111](https://github.com/JacksonSibold/linux-nt-personality/issues/111) |

The release rungs v0.5.0 (25 criteria passing), v0.6.0 (50), v0.7.0 (100),
v0.8.0 (200), v0.9.0 (300) and v1.0.0 (423 of 423) are milestones 7 to 12.
The v0.5.0 and v1.0.0 targets were given in the planning request. The intermediate
rungs are proposals and need the owner's confirmation. The rung targets were
given in the planning request for this roadmap, not in a repository document.
No release is made from this roadmap, and the existing tags v0.1.0 to v0.4.0
are not changed.

## Good first issues

- [#54](https://github.com/JacksonSibold/linux-nt-personality/issues/54): `snprintf` return value.
- [#99](https://github.com/JacksonSibold/linux-nt-personality/issues/99): reconcile the golden status of `probe_sharemode_direct`.
- [#112](https://github.com/JacksonSibold/linux-nt-personality/issues/112): the kernel-nt summary table.
- [#113](https://github.com/JacksonSibold/linux-nt-personality/issues/113): the decision-record count in the review index.

Each one names its source file, its acceptance criteria and the command that
verifies it.
