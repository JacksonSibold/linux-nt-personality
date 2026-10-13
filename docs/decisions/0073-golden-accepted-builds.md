# 0073: Golden corpus accepts a list of builds (option 3)

Status: Accepted. Implemented in 97fa1d4 (accepted-builds list, checker, size policy), 36ab49f (BLOCKED-ON-GOLDEN report) and 36d570c (status docs).
Finding: none in `docs/review/`. Golden policy decision in `docs/status/GOLDEN.md` sections 2 to 4, delegated by the owner to the team.
Date: 2026-10-13

## Context

The Windows oracle corpus is captured on the host, and the policy pinned it to `26200.9168`. The only corpus available is captured on `26200.9457` (`tests/differential/golden/windows-26200.9457/`). The pinned build could not be captured here. Without a decision, no reader consumed the corpus, and the lint gate could not pass.

The gate also depended on a pin that the checker enforced in several places, plus a fallback that took the newest version directory. That fallback could accept an unlisted build without a decision recorded.

Three options were on the table (GOLDEN.md section 4): keep the pin and capture on 26200.9168, re-pin to the host build, or accept a list of builds.

## Decision

Option 3. Accept a list of builds, recorded in `tests/differential/golden/ACCEPTED_BUILDS.json`:

- A golden directory is accepted only if its build is listed, and its `version.json` and `manifest.json` sha256 values match the entry. The `version.json` oracle must equal the entry's provenance (`host-captured` or `pinned-reference`), and the manifest host build must equal the build.
- The golden build must still equal the running host build. That build-drift guard is kept. A listed build is therefore usable on a host of that build, and the list does not allow mixing builds in one comparison.
- The `NTPERS_GOLDEN_BUILD` environment opt-in and the pinned, `latest` and newest-directory fallbacks are removed. There is no environment bypass.
- A probe may be listed in an entry's `known_gaps` with a reason code (`unsafe-on-host`, `too-large-kept-out`, `not-portable`). That waives only the absence of its golden. Any criterion whose registry entry declares `requires_golden` on that probe is reported **BLOCKED-ON-GOLDEN** and is never passed. A gap whose golden is present fails the gate.
- Size policy, owner decision of 2026-10-13: a large golden is stored gzip-compressed when the compressed size is under 512 KiB, and its decompressed sha256 is checked against the manifest. Otherwise a summary (`ntpers-golden-summary/1`) stands in under the probe name. A summary must validate against the manifest, and its full output is compared when it is available locally. A summary that is empty, placeholder or missing fields is rejected.

## Options considered for the size policy

1. Commit every golden as text. Rejected: a 9 MB output would break the 64 KiB rule.
2. Gzip only, with no summaries. Rejected for the largest outputs: the compressed size can still exceed the limit, and a summary can carry the same hash evidence.
3. Gzip under 512 KiB, else a verified summary. Chosen.

## Consequences

- The corpus gets two more probes as goldens (`probe_sharemode`, `probe_virtualquery_direct`, both gzipped). Four of 22 policy probes remain without a golden (`probe_ntstatus_direct`, `probe_path`, `probe_path_direct`, `probe_sharemode_direct`). They are listed as known gaps.
- No summary has been generated. The raw outputs for the three large probes are not on local disk, so their summaries cannot be produced without a recapture. They are not fabricated.
- The lint gate still fails, on purpose. Four goldens (`probe_handles_direct`, `probe_process_thread_direct`, `probe_sync_direct`, `probe_virtualquery_direct`) carry a hard-coded `windows_build` of `26200.9168` in the probe source, and the build check rejects them. Clearing this needs a probe-source change that reports the host build, then a recapture. That is a separate decision.
- A new BLOCKED-ON-GOLDEN state exists in `dodrun.py`. It never counts as PASS. No registry entry declares `requires_golden` yet, so the blocked count is 0 today.
- Adding a build to the list is a reviewed change to a file with hashes. A new capture cannot be accepted by editing a flag.

## Verification

- `tests/unit/test_golden_accepted_builds.py`: accepted build passes; unlisted host build fails; env var does not bypass; manifest and version hash mismatch fail; provenance mismatch fails; schema rejects bad entries and duplicates; known gap respected; gap with a present golden fails; blocked criterion reported and never run; summary valid, wrong-hash, missing-field and placeholder cases; gzip verified by manifest hash; shipped list verifies against the committed corpus.
- Results, with commands and exit codes, are in `docs/status/GOLDEN.md` section 8.4 and `docs/status/CRITERIA-PROGRESS.md`.
