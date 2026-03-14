# Decision: ETag-Based Cache Reuse is Opt-In via URI Parameter

**Date:** 2026-03-10 | **From:** Aragorn (Implementation)

## Decision

Cache persistence is **opt-in** via `cache_reuse=1` URI parameter. When disabled (default), behavior is identical to pre-change: mkstemp creates random cache file names, xClose unlinks them.

## Details

- **Cache naming:** FNV-1a hash of `account:container:blobName` → deterministic path `{cache_dir}/sqlite-objs-{16hex}.cache`
- **ETag sidecar:** Same base path with `.etag` extension. Written only on clean close (no dirty pages + valid ETag).
- **Validation on open:** Stored ETag compared against live blob ETag from `blob_get_properties`. Also verifies cached file size matches blob size.
- **Crash safety:** No ETag sidecar is written if dirty pages exist. On crash, next open sees no sidecar → full download. No data corruption path.

## Rationale

Opt-in avoids surprises for users who don't expect persistent files in their cache directory. The ETag + size double-check prevents serving stale data. FNV-1a is non-cryptographic but sufficient for cache naming uniqueness.

## Impact on Team

- **Samwise (Tests):** New tests needed for cache reuse paths — ETag match/mismatch, stale cache cleanup, `cache_reuse=0` regression.
- **Frodo (Azure Client):** No changes needed. Relies on existing `blob_get_properties` ETag population.
