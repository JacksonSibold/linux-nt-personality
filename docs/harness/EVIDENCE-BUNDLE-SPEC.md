# Evidence bundle specification

This document describes the evidence bundle layout that the harness checks today,
the gaps against the signed-bundle criteria (`DOD-EVI-B001` to `DOD-EVI-B005`), and
the target layout those gaps require. It creates no bundle, signature or timestamp.
Signing and timestamping need the build key and a time-stamp authority that this
repository does not hold.

## Current layout (checked by the integrity lint)

`tests/harness/lint_integrity.py`, check `check_evidence_bundle_hash_covers_all_files`,
reads each bundle under `reports/evidence/<bundle>/`:

| File | Role |
|---|---|
| `evidence.json` | Holds `run_id` and `manifest`, a map from each bundle file name to its entry. Not covered by the hash (the lint says so). |
| `hash.txt` | SHA-256 over the contents of the manifest's files, in manifest order, followed by the bytes of `run_id`. |
| `run_id.txt` | Must equal `run_id` in `evidence.json`. |
| files named in `manifest` | The evidence itself. Any other file in the bundle is an error. |

Bundles without `evidence.json` are skipped. No bundle exists in this repository,
so the check passes vacuously.

dodrun does not write bundles. It writes `reports/results-<UTC>.json` and a summary
text file, both unsigned. The results file records the state of each criterion; it
is not an evidence bundle.

## Gaps against the signed-bundle criteria

1. No signature. Nothing is signed by the build key. `DOD-EVI-B001` requires one.
2. No time-stamp. There is no RFC3161 token. `DOD-EVI-B001` requires one.
3. `evidence.json` is not hash-covered. A change to the manifest would not be caught
   by `hash.txt`.
4. No write-once archive. Bundles are written in place.
5. dodrun does not produce bundles, so `DOD-EVI-B001` ("every P0/P1 criterion run
   produces a bundle") cannot hold.
6. Replay (`DOD-EVI-B002`), corpus pinning and canary (`DOD-EVI-B003`), mutation
   campaign (`DOD-EVI-B005`) and the per-case export reconciliation (`DOD-EVI-B004`)
   have no tooling in the repository.

## Target layout (not implemented)

```
reports/evidence/<criterion-id>/<run_id>/
  manifest.json       { "schema": "ntpers-evidence/2", "criterion", "run_id",
                        "created_utc", "host", "files": [{"path", "sha256", "bytes"}],
                        "signer_key_id", "timestamp_authority" }
  result.json         the dodrun record for the criterion
  stdout.txt, stderr.txt
  signature.sig       detached signature over manifest.json, by the build key
  timestamp.tsr       RFC3161 token over the SHA-256 of manifest.json
```

`manifest.json` lists `result.json` and the outputs. The signature and the time-stamp
cover `manifest.json` itself, so the manifest cannot be changed without detection. Verification recomputes each listed SHA-256, checks
`signature.sig` against the published public key, and checks `timestamp.tsr`
against the authority's certificate. A bundle that fails any step is invalid.

## Validity rule

A bundle is valid for `DOD-EVI-B001` only with a verified signature and a verified
time-stamp. A bundle without them is not valid, and a criterion whose only record is
such a bundle is not PASS. Today there are no valid bundles.

## Owner inputs (not in the repository)

- The build signing key pair. The owner generates and holds it. Its public part is
  published with the release.
- The time-stamp authority. The owner chooses it.
- The archive location for write-once storage.

Until these exist, `DOD-EVI-B001` to `DOD-EVI-B005` are BLOCKED on owner input.
