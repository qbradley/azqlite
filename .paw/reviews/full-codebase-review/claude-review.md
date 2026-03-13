# Claude Review: Security, Memory & Crash Safety

**Reviewer:** Claude (Security & Systems Focus)
**Scope:** All files in `src/`, `test/`, `benchmark/` (excluding upstream `sqlite3.c`/`sqlite3.h`)
**Date:** 2025-07-15

---

## Executive Summary

The azqlite codebase is generally well-architected with a clean VFS ↔ Azure abstraction boundary, proper retry logic, and good test coverage via mocks. However, this review uncovered **4 critical**, **11 high**, **16 medium**, and **10 low** findings across security, memory safety, error handling, and crash integrity domains.

The most concerning findings are:
1. **Integer overflow in buffer size computation** (`azure_buffer_append`) that could lead to heap corruption
2. **Missing dirty bitmap bounds checking** that enables out-of-bounds write
3. **`memset` scrubbing likely optimized away** by the compiler, leaving secrets in memory
4. **Unchecked `snprintf` overflow** in auth signing can silently truncate the StringToSign, producing invalid authentication signatures

Most production code (`azqlite_vfs.c`, `azure_client.c`) handles errors well, but the pattern breaks down in test code where ~60+ return values go unchecked.

---

## Critical Findings

### C1. Integer overflow in `azure_buffer_append` → heap corruption
**File:** `src/azure_client.c:55-57`
**Severity:** CRITICAL

```c
if (buf->size + len > buf->capacity) {
    size_t new_cap = (buf->capacity == 0) ? 4096 : buf->capacity * 2;
    while (new_cap < buf->size + len) new_cap *= 2;
```

Both `buf->size + len` and `new_cap *= 2` can overflow `size_t` without any check. If an attacker-controlled Azure response triggers a large enough response body, `new_cap` wraps to zero or a small value, `realloc` allocates a tiny buffer, and the subsequent `memcpy` at line 63 writes out of bounds.

Additionally, `size * nmemb` at `curl_write_cb` line 76 can overflow before being passed to `azure_buffer_append`.

**Fix:** Add overflow checks:
```c
if (len > SIZE_MAX - buf->size) return -1;  // overflow check
size_t needed = buf->size + len;
// ...
if (new_cap > SIZE_MAX / 2) return -1;  // prevent wrap
new_cap *= 2;
```

---

### C2. Dirty bitmap out-of-bounds write in `dirtyMarkPage`
**File:** `src/azqlite_vfs.c:215-223`
**Severity:** CRITICAL

```c
static void dirtyMarkPage(azqliteFile *p, sqlite3_int64 offset) {
    if (!p->aDirty || p->pageSize <= 0) return;
    int pageIdx = (int)(offset / p->pageSize);
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    if (!(p->aDirty[byteIdx] & (1 << bitIdx))) {
        p->aDirty[byteIdx] |= (1 << bitIdx);
```

There is **no bounds check** on `byteIdx` against `p->nDirtyAlloc`. If the buffer grows via `bufferEnsure` but the dirty bitmap reallocation fails at line 249-250 (and `dirtyEnsureCapacity` returns `SQLITE_NOMEM` which `bufferEnsure` does return, but `xWrite` at line 529 calls `dirtyMarkPage` after already writing data), the bitmap may be too small for the current file size. A write near the end of an expanded file will index past `p->aDirty` causing heap corruption.

Similarly, `dirtyIsPageDirty` at line 226-230 has the same issue — no bounds check on `byteIdx`.

**Fix:** Add bounds check:
```c
if (byteIdx < 0 || byteIdx >= p->nDirtyAlloc) return;
```

---

### C3. `snprintf` overflow in `azure_auth_sign_request` silently truncates StringToSign
**File:** `src/azure_auth.c:176-192, 233`
**Severity:** CRITICAL

The `string_to_sign` buffer is 4096 bytes. The code uses `p += snprintf(p, (size_t)(end - p), ...)` throughout, but **if snprintf returns a value >= the available space (meaning truncation occurred), `p` advances past `end`**, making `(end - p)` negative, which when cast to `size_t` becomes a huge number, causing all subsequent writes to overflow.

Specifically: `snprintf` returns the number of characters that *would have been written* (not the number actually written). If the StringToSign exceeds 4096 bytes (possible with many x-ms-* headers or very long query strings), `p` jumps past `end`, and subsequent snprintf calls get a massive `size_t` argument.

The canonicalized resource at line 233 appends `/account/path` without a length check — long blob names compound this.

**Fix:** After each `snprintf`, clamp `p`:
```c
int n = snprintf(p, (size_t)(end - p), ...);
if (n < 0 || p + n >= end) { free(sorted); return AZURE_ERR_INVALID_ARG; }
p += n;
```

---

### C4. `memset` key scrubbing optimized away by compiler
**File:** `src/azure_client.c:1744-1746`
**Severity:** CRITICAL

```c
memset(client->key_raw, 0, sizeof(client->key_raw));
memset(client->key_b64, 0, sizeof(client->key_b64));
memset(client->sas_token, 0, sizeof(client->sas_token));
free(client);
```

The compiler is allowed to optimize away `memset` calls on memory that is immediately freed, since the writes are "dead stores" from the compiler's perspective. This means the shared key and SAS token may remain in memory after `azure_client_destroy`.

**Fix:** Use `explicit_bzero()` (POSIX), `memset_s()` (C11 Annex K), or a volatile-based scrubbing pattern:
```c
explicit_bzero(client->key_raw, sizeof(client->key_raw));
explicit_bzero(client->key_b64, sizeof(client->key_b64));
explicit_bzero(client->sas_token, sizeof(client->sas_token));
```

---

## High Findings

### H1. Integer overflow in `delay_ms * 1000` in `azure_retry_sleep_ms`
**File:** `src/azure_error.c:201`
**Severity:** HIGH

```c
usleep((unsigned int)(delay_ms * 1000));
```

`delay_ms` can be up to `AZURE_RETRY_MAX_MS` (30000). `30000 * 1000 = 30,000,000` fits in `int`, but the intermediate `int` multiplication could overflow on platforms where `AZURE_RETRY_MAX_MS` is larger or if `retry_after_secs` provides a huge value from a malicious server (the cap at line 192 prevents this for now, but the pattern is fragile).

More critically, if `retry_after_secs` from a malicious Azure response is `INT_MAX / 1000 + 1`, then `retry_after_secs * 1000` at line 184 overflows before the cap check at line 192.

**Fix:** Cast before multiplication: `usleep((useconds_t)delay_ms * 1000U);` and add overflow check to `retry_after_secs * 1000`.

---

### H2. `BIO_write` / `BIO_flush` return values unchecked in base64 encode
**File:** `src/azure_auth.c:46-47`
**Severity:** HIGH

```c
BIO_write(b64, input, (int)input_len);
BIO_flush(b64);
```

Both `BIO_write` and `BIO_flush` can fail (return ≤ 0), but their return values are ignored. On failure, `BIO_get_mem_ptr` at line 50 may return garbage data, producing invalid base64 that silently corrupts authentication signatures.

**Fix:** Check both return values and return error on failure.

---

### H3. `HMAC()` deprecated and `key_len` truncated to `int`
**File:** `src/azure_auth.c:98`
**Severity:** HIGH

```c
uint8_t *result = HMAC(EVP_sha256(), key, (int)key_len, ...);
```

1. `HMAC()` is deprecated since OpenSSL 3.0 in favor of `EVP_MAC_*` APIs. It may be removed in future OpenSSL versions.
2. `key_len` is cast to `int`, silently truncating if key > 2 GiB (unlikely but the cast masks real issues).

**Fix:** Migrate to `EVP_MAC_*` API for OpenSSL 3.0+ compatibility.

---

### H4. Journal truncate `memset` size calculation can underflow
**File:** `src/azqlite_vfs.c:586-587`
**Severity:** HIGH

```c
memset(p->aJrnlData + size, 0,
       (size_t)(p->nJrnlAlloc - size < 0 ? 0 : p->nJrnlAlloc - size));
```

This comparison `p->nJrnlAlloc - size < 0` is between two `sqlite3_int64` values and should work correctly. However, the expression is fragile — if `size` were somehow negative (which it won't be from SQLite, but defensively), the subtraction underflows. The ternary makes the intent clear but the better approach is `max(0, p->nJrnlAlloc - size)`.

More importantly: when `size > 0` and `p->nJrnlAlloc > size`, the code zeros `nJrnlAlloc - size` bytes starting at offset `size`, but this could extend into uninitialized realloc territory if `nJrnlAlloc` was never fully written. This isn't a security issue but can leak uninitialized heap data if the journal is read back.

---

### H5. `rand()` used for cryptographic-adjacent purposes (jitter + lease IDs)
**File:** `src/azure_error.c:189`, `src/azure_client.c:1037`
**Severity:** HIGH

`rand()` is not cryptographically secure and not thread-safe. In the retry jitter context, predictable jitter enables targeted timing attacks. In the mock's `generate_lease_id` (test code), it creates predictable "lease IDs" which is fine for tests but worth noting the pattern.

More importantly, `srand((unsigned)time(NULL))` at line 1037 of `azure_client.c` seeds the PRNG with second-resolution time, making the sequence predictable.

**Fix:** Use `arc4random()` on macOS/BSD or `/dev/urandom` for jitter.

---

### H6. `curl_slist_append` return values never checked
**File:** `src/azure_client.c:308-358` (all `curl_slist_append` calls)
**Severity:** HIGH

Every call to `curl_slist_append` can return NULL on allocation failure, but none are checked. If any return NULL, the entire header list is lost (the previous list is freed internally by curl), and subsequent `curl_slist_append` calls receive NULL, causing undefined behavior.

There are approximately 15 unchecked `curl_slist_append` calls in `execute_single` and another ~10 in `batch_init_easy`.

**Fix:** Check each return value; on NULL, free the list and return `AZURE_ERR_NOMEM`.

---

### H7. Global mutable state is not thread-safe
**File:** `src/azqlite_vfs.c:33-51` (g_debug_timing, g_xread_count, etc.), `src/azqlite_vfs.c:1683` (g_vfsData)
**Severity:** HIGH

`g_vfsData`, `g_azqliteVfs`, `g_debug_timing`, `g_xread_count`, and `g_xread_journal_count` are all global mutable state with no synchronization. While SQLite's btree mutex serializes most VFS calls, `xAccess` and `xDelete` can be called from any thread. The `journalCacheState` field in `g_vfsData` is particularly concerning — concurrent `xAccess` and `xDelete` calls could race on this field.

**Fix:** Document single-threaded assumption or add atomics/mutex for `journalCacheState`.

---

### H8. `http_status` narrowed from `long` to `int` without range check
**File:** `src/azure_client.c:442`
**Severity:** HIGH

```c
err->http_status = http_status;
```

`http_status` is `long` (from `curl_easy_getinfo`), but `azure_error_t.http_status` is `int`. On platforms where `long` > `int`, values > INT_MAX cause implementation-defined behavior.

**Fix:** Clamp before assignment: `err->http_status = (int)(http_status > INT_MAX ? INT_MAX : http_status);`

---

### H9. WAL append retry can duplicate data
**File:** `src/azqlite_vfs.c:746-761`
**Severity:** HIGH

In the WAL incremental sync path, if `append_blob_append` fails mid-way through chunked appending (e.g., chunks 1-3 succeed, chunk 4 fails), the VFS returns `SQLITE_IOERR_FSYNC` without updating `nWalSynced`. On the next `xSync`, the code will re-send chunks 1-3 again (since `nWalSynced` wasn't advanced), causing **duplicate data** in the append blob.

Append blobs are **append-only** — there's no way to "undo" the first 3 chunks. The WAL will contain duplicate frame data, which would corrupt the database on replay.

**Fix:** Update `nWalSynced` after each successful chunk, or delete+recreate the append blob on partial failure.

---

### H10. No `SQLITE_OPEN_WAL` in `0x0000FF00` mask
**File:** `src/azqlite_vfs.c:1232-1233`
**Severity:** HIGH

```c
p->eFileType = flags & 0x0000FF00;  /* Extract type flags */
if (isWal) p->eFileType = SQLITE_OPEN_WAL;
```

`SQLITE_OPEN_WAL` is defined as `0x00080000` in SQLite, which is **outside** the `0x0000FF00` mask. The code handles this with a special case on line 1233, but if this special case were ever removed or if the `isWal` variable check were bypassed, the WAL file would be treated as a different type. This fragile masking pattern should be replaced.

**Fix:** Use explicit type assignment rather than bit masking:
```c
if (isWal) p->eFileType = SQLITE_OPEN_WAL;
else if (isMainJournal) p->eFileType = SQLITE_OPEN_MAIN_JOURNAL;
else p->eFileType = SQLITE_OPEN_MAIN_DB;
```

---

### H11. `xRead` doesn't validate `src` is non-NULL before `memcpy`
**File:** `src/azqlite_vfs.c:449-477`
**Severity:** HIGH

```c
if (p->eFileType == SQLITE_OPEN_WAL) {
    src = p->aWalData;
    srcLen = p->nWalData;
```

If a WAL file was opened but `aWalData` was never allocated (e.g., `walBufferEnsure` was never called because no write happened), `src` is NULL. Then `memcpy(pBuf, src + iOfst, iAmt)` at line 470 dereferences NULL. Same for `aJrnlData` and `aData`.

While SQLite normally wouldn't read from a file it hasn't written to, the contract says `xRead` must handle any call gracefully.

**Fix:** Add `if (!src) { memset(pBuf, 0, iAmt); return SQLITE_IOERR_SHORT_READ; }` after selecting src.

---

## Medium Findings

### M1. `azure_buffer_append` new_cap can overflow via doubling loop
**File:** `src/azure_client.c:57`
**Severity:** MEDIUM
The `while (new_cap < buf->size + len) new_cap *= 2;` loop can overflow `size_t` silently, producing a small `new_cap` that fails to meet the requirement. Already subsumed by C1, but the doubling loop specifically needs an overflow guard.

### M2. `strncpy` without explicit NUL-termination in `azure_client_create`
**File:** `src/azure_client.c:1673-1674`
**Severity:** MEDIUM
```c
strncpy(c->account, account, sizeof(c->account) - 1);
strncpy(c->container, container, sizeof(c->container) - 1);
```
`strncpy` doesn't NUL-terminate if source length >= limit. While `calloc` zeroes the buffer, the code should defensively NUL-terminate: `c->account[sizeof(c->account)-1] = '\0';`. Same applies to lines 1679, 1688, 1691.

### M3. `leaseRenewIfNeeded` uses `time()` which has second resolution
**File:** `src/azqlite_vfs.c:339-340`
**Severity:** MEDIUM
Using `time()` and `difftime()` with second-resolution means the lease renewal timing has ~1s jitter. For a 30s lease with 15s renewal threshold, this leaves a narrow margin if many writes occur near the boundary. Consider using `gettimeofday()` or `clock_gettime()` for sub-second precision.

### M4. `dirtyBitmapSize` integer truncation from `sqlite3_int64` to `int`
**File:** `src/azqlite_vfs.c:211`
**Severity:** MEDIUM
```c
int nPages = (int)((fileSize + pageSize - 1) / pageSize);
```
For files > ~8 TiB with 4096-byte pages, `nPages` exceeds `INT_MAX`, causing undefined behavior. While Azure page blobs max at 8 TiB, this is close enough to warrant a check.

### M5. `extract_xml_tag` doesn't handle CDATA, entities, or nested tags
**File:** `src/azure_error.c:60-80`
**Severity:** MEDIUM
The XML parser uses `strstr` to find tags. A crafted error response containing `<Code><Code>Malicious</Code></Code>` would extract `<Code>Malicious` as the code. While Azure controls the error format, defense-in-depth suggests at least rejecting `<` in extracted values.

### M6. Potential uninitialized `aerr` use in sequential sync path
**File:** `src/azqlite_vfs.c:933`
**Severity:** MEDIUM
```c
if (aerr.etag[0] != '\0') {
```
If `nRanges == 0` (which is blocked by the check at line 795), `aerr` would be uninitialized here. In the actual code path, the `for` loop always runs at least once, but `aerr` is re-initialized inside the loop at line 904, and the check at line 933 uses the `aerr` from the **last** iteration, which is correct. However, if no ranges are written (which line 795 prevents), this would access uninitialized memory.

### M7. `xWrite` integer overflow: `iOfst + iAmt`
**File:** `src/azqlite_vfs.c:492, 506, 515`
**Severity:** MEDIUM
```c
sqlite3_int64 end = iOfst + iAmt;
```
If `iOfst` is near `INT64_MAX`, adding `iAmt` overflows. While SQLite limits file sizes, the VFS layer should validate inputs.

### M8. `azqliteFullPathname` doesn't reject embedded NUL bytes
**File:** `src/azqlite_vfs.c:1588-1611`
**Severity:** MEDIUM
`strlen(p)` stops at the first NUL, but if the input contains embedded NULs, the name used by Azure APIs will be truncated. This could allow a "path confusion" attack where SQLite sees one blob name but Azure operates on a different one. Unlikely in practice since SQLite normalizes names.

### M9. `xClose` attempts `azqliteSync` on failure paths
**File:** `src/azqlite_vfs.c:400-405`
**Severity:** MEDIUM
If `xSync` fails during `xClose`, the error is returned to SQLite. But `xClose` is often called during error handling (e.g., after a failed commit). SQLite may not expect `xClose` to fail and may not handle the error gracefully. The lease release on lines 409-420 is correctly best-effort, but the sync should arguably also be best-effort in `xClose`.

### M10. `azure_compute_retry_delay` bit shift overflow
**File:** `src/azure_error.c:187`
**Severity:** MEDIUM
```c
delay_ms = AZURE_RETRY_BASE_MS * (1 << attempt);
```
If `attempt >= 31` (not possible with `AZURE_MAX_RETRIES=5`, but the function is public), `1 << attempt` is undefined behavior in C. Even with `attempt=5`, `500 * 32 = 16000` which is fine, but the function's API allows arbitrary `attempt` values.

### M11. `qsort` with `strcasecmp` comparison for authentication may not match Azure's expectations
**File:** `src/azure_auth.c:121-123`
**Severity:** MEDIUM
Azure's canonicalization requires headers sorted by **lowercase name**. Using `strcasecmp` for sorting should produce the same order, but locale-dependent `strcasecmp` implementations could differ from Azure's ASCII-only comparison.

### M12. WAL `walNeedFullResync` delete+recreate is not atomic
**File:** `src/azqlite_vfs.c:705-720`
**Severity:** MEDIUM
The full resync path deletes the append blob and recreates it. If the process crashes between delete and recreate, the WAL data is lost. Since WAL mode in azqlite requires `PRAGMA locking_mode=EXCLUSIVE`, only one writer exists, but a crash at this point loses committed WAL frames that haven't been checkpointed.

### M13. Query parameter limit of 32 pairs silently drops excess
**File:** `src/azure_auth.c:240-247`
**Severity:** MEDIUM
`char *pairs[32]` with `npairs < 32` limit means queries with more than 32 parameters are silently truncated from the StringToSign, causing authentication failures that would be very hard to debug. Should log a warning or return an error.

### M14. `g_vfsData` is static — VFS cannot be unregistered cleanly
**File:** `src/azqlite_vfs.c:1683`
**Severity:** MEDIUM
`g_vfsData` and `g_azqliteVfs` are static globals. If `azqlite_vfs_register_with_config` is called twice, the first `client` is leaked (the old `g_vfsData.client` is overwritten without being destroyed). There is no `azqlite_vfs_unregister` function.

### M15. `xOpen` WAL failure path doesn't free `aWalData`
**File:** `src/azqlite_vfs.c:1438-1439`
**Severity:** MEDIUM
```c
free(p->aWalData);
p->aWalData = NULL;
free(p->zBlobName);
```
This cleanup at line 1438-1441 only runs if `walBufferEnsure` fails, but there are earlier paths in WAL open that don't reach this cleanup (e.g., if the blob_exists check fails at line 1414).

### M16. `http_status` cast from `long` to `int` in batch write path
**File:** `src/azure_client.c:1380, 1390`
**Severity:** MEDIUM
Same as H8 but in the batch write code path. Long HTTP status codes truncated to int.

---

## Low Findings

### L1. Unused `#include <assert.h>` in `azqlite_vfs.c`
**File:** `src/azqlite_vfs.c:28`
**Severity:** LOW
`assert.h` is included but `assert()` is never called. Should be removed to avoid confusion about whether assertions are used.

### L2. `xFileControl` unreachable code
**File:** `src/azqlite_vfs.c:1126-1127`
**Severity:** LOW
```c
    return SQLITE_NOTFOUND;
}
```
The `return SQLITE_NOTFOUND` at line 1126 is unreachable because the default case at line 1124 already returns.

### L3. `azure_buffer_init` doesn't check for NULL parameter
**File:** `src/azure_client.h:106-110`
**Severity:** LOW
Unlike `azure_error_init` which checks for NULL, `azure_buffer_init` unconditionally dereferences `buf`. Should add a NULL guard for consistency.

### L4. `g_http_request_count` and related counters are global non-atomic
**File:** `src/azure_client.c:45-47`
**Severity:** LOW
These debug counters are not thread-safe, but since they're only used for debug timing output, the impact is cosmetic.

### L5. `azure_err_str` default case may miss new enum values
**File:** `src/azure_error.c:26-44`
**Severity:** LOW
The switch has no `default` label inside the switch body (there is a fallthrough default at line 44). If a new `azure_err_t` value is added, the compiler won't warn about the missing case. Add `-Wswitch-enum` to build flags.

### L6. `execute_single` local buffers are large (multiple ~4096-byte arrays on stack)
**File:** `src/azure_client.c:210, 306-307, 259`
**Severity:** LOW
`url[4096]`, `h_date[256]`, `h_version[128]`, `h_auth[600]`, etc. Combined stack usage in `execute_single` is ~6KB. Combined with `batch_init_easy` calling the same pattern, stack depth in recursive/nested scenarios could approach limits.

### L7. Test code: ~60+ unchecked return values from SQLite/Azure operations
**File:** Various test files
**Severity:** LOW (test code)
While the project directive says every return code must be checked, test code pervasively calls mock operations (e.g., `g_ops->page_blob_create()`) without checking return values in setup phases. These are setup calls where failure would cause subsequent assertions to fail anyway, but the pattern violates the stated policy. Key examples: `test_vfs.c:808,814,821,852,869`, `test_wal.c:292,324,331`, `test_coalesce.c:133-135,285,290`, `test_integration.c:370,378,425,447-454`.

### L8. Benchmark `tpcc_load.c` uses `sprintf` (unbounded)
**File:** `benchmark/tpcc/tpcc_load.c:26, 101, 198`
**Severity:** LOW (benchmark code)
Multiple `sprintf` calls without bounds checking, including `random_zip()` which writes into a caller-provided buffer with no size parameter. Not production code, but buffer overflows in benchmarks can corrupt test results or crash.

### L9. `malloc` return unchecked in `tpcc.c:60`
**File:** `benchmark/tpcc/tpcc.c:60`
**Severity:** LOW (benchmark code)
`stats->latencies = malloc(...)` without NULL check; dereferenced at line 76-77.

### L10. `azqlite_shell.c` relies on `#define main shell_main` hack
**File:** `src/azqlite_shell.c:33-35`
**Severity:** LOW
This `#define main shell_main` / `#include "shell.c"` / `#undef main` pattern is brittle and will break if `shell.c` ever references `main` in a string, comment, or other context. Consider using the official `SQLITE_SHELL_INIT_PROC` mechanism instead.

---

## File-by-File Analysis

### src/azqlite_vfs.c

**Overall:** Well-structured with clear separation of concerns. Buffer management is careful with geometric growth. The dirty bitmap optimization is clever. Main risks are bounds checking on the dirty bitmap and the WAL partial-sync data duplication issue.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 211 | MEDIUM | `nPages` integer truncation from int64 to int |
| 215-223 | CRITICAL | Missing bounds check on dirty bitmap index |
| 226-230 | CRITICAL | Same issue in `dirtyIsPageDirty` |
| 400-405 | MEDIUM | `xClose` calls `xSync` which may fail; error handling unclear |
| 449-477 | HIGH | `src` may be NULL if buffer never allocated |
| 492, 506, 515 | MEDIUM | `iOfst + iAmt` overflow potential |
| 586-587 | HIGH | Fragile `memset` size expression |
| 746-761 | HIGH | Partial WAL append creates duplicate data on retry |
| 933 | MEDIUM | `aerr` read depends on loop always executing |
| 1035-1036 | OK | `lease_release` return intentionally ignored — documented |
| 1126 | LOW | Unreachable return statement |
| 1232-1233 | HIGH | Fragile bit-mask doesn't capture WAL flag |
| 1683 | MEDIUM | Static globals prevent re-registration and lack cleanup |

### src/azure_client.c

**Overall:** Solid HTTP client implementation with proper retry logic and connection reuse. The batch write implementation using `curl_multi` is well-designed. Main concern is unchecked `curl_slist_append` returns.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 55-57 | CRITICAL | Integer overflow in buffer size computation |
| 76 | CRITICAL | `size * nmemb` multiplication overflow |
| 308-358 | HIGH | All `curl_slist_append` returns unchecked |
| 442 | HIGH | `long` to `int` narrowing of HTTP status |
| 1037 | HIGH | `srand(time(NULL))` is predictable |
| 1673-1674 | MEDIUM | `strncpy` without explicit NUL termination |
| 1744-1746 | CRITICAL | `memset` scrubbing likely optimized away |

### src/azure_auth.c

**Overall:** Authentication implementation follows Azure's StringToSign spec correctly. The main risk is snprintf overflow in the StringToSign buffer and unchecked BIO operations.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 46-47 | HIGH | `BIO_write`/`BIO_flush` return values unchecked |
| 98 | HIGH | `HMAC()` deprecated; key_len truncated to int |
| 121-123 | MEDIUM | Locale-dependent `strcasecmp` may not match Azure |
| 176-192 | CRITICAL | `snprintf` overflow advances pointer past buffer end |
| 200-201 | OK | `malloc` checked for NULL — good |
| 236-247 | MEDIUM | Silent truncation of >32 query parameters |

### src/azure_error.c

**Overall:** Clean, straightforward error classification and retry logic. The XML parser is appropriately minimal for the Azure error response format.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 60-80 | MEDIUM | XML parser doesn't handle injection/CDATA |
| 187 | MEDIUM | Bit shift undefined for large `attempt` values |
| 189 | HIGH | `rand()` not thread-safe or cryptographically secure |
| 201 | HIGH | `delay_ms * 1000` potential integer overflow |

### src/azure_client.h

**Overall:** Clean API surface. Inline helpers are safe. No issues except missing NULL check in `azure_buffer_init`.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 106-110 | LOW | `azure_buffer_init` doesn't check for NULL |

### src/azure_client_impl.h

**Overall:** No issues. Internal header with appropriate constant definitions.

### src/azure_client_stub.c

**Overall:** Clean stub implementation. All stubs properly set error state and return appropriate codes.

### src/azqlite.h

**Overall:** Clean public header. No issues.

### src/azqlite_shell.c

**Overall:** Functional but uses the brittle `#define main` / `#include shell.c` pattern.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 33-35 | LOW | Brittle main redefinition pattern |

### test/mock_azure_ops.c

**Overall:** Comprehensive mock with proper state machine for leases. Key issues are `strcpy` without bounds checking and missing NULL checks on some buffers.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| ~149 | MEDIUM | `strncpy` without explicit NUL termination |
| ~499-510 | HIGH | `strcpy` without buffer size validation |
| ~312 | MEDIUM | Integer overflow in `offset + len` |
| ~358-365 | MEDIUM | `realloc` failure leaks original buffer |

### test/mock_azure_ops.h

**Overall:** Clean header with appropriate interface for testing.

### test/test_harness.h

**Overall:** Simple and functional test framework using `setjmp`/`longjmp`. The `longjmp` approach means cleanup code after an assertion failure won't run, which could leak resources in tests, but this is acceptable for a test framework.

### test/test_main.c

**Overall:** Clean test runner. No issues.

### test/test_vfs.c

**Overall:** Thorough coverage of mock operations, VFS integration, and failure injection. Many unchecked return values in setup code.

| Area | Severity | Finding |
|------|----------|---------|
| Setup calls | LOW | ~30+ unchecked `page_blob_create`/`page_blob_write` in test setup |
| VFS integration tests | LOW | `sqlite3_close`/`sqlite3_finalize` not always checked |

### test/test_wal.c

**Overall:** Good WAL-specific test coverage including chunking and crash recovery scenarios.

| Area | Severity | Finding |
|------|----------|---------|
| `wal_exec()` calls | LOW | ~15+ unchecked return values |
| Cleanup | LOW | `wal_base_ops` not cleared after tests |

### test/test_coalesce.c

**Overall:** Focused coalesce testing with good coverage of edge cases and batch operations.

| Area | Severity | Finding |
|------|----------|---------|
| `sqlite3_prepare_v2` calls | LOW | ~10 unchecked return values |
| `co_disable_batch()` | MEDIUM | Not called in all cleanup paths (lines ~650, 694) |

### test/test_integration.c

**Overall:** Integration tests that exercise the full VFS through SQLite APIs. Good coverage.

| Area | Severity | Finding |
|------|----------|---------|
| `sqlite3_exec`/`sqlite3_step` | LOW | ~15 unchecked return values |
| `errmsg` cleanup | LOW | Some paths may not free `errmsg` |

### test/test_azure_client.c

**Overall:** Good coverage of azure_client types and operations through mock.

### benchmark/benchmark.c

**Overall:** Simple benchmark runner. Minor issues with `atoi` and unchecked `unlink`.

### benchmark/speedtest1_wrapper.c

**Overall:** Clean wrapper. No issues.

### benchmark/tpcc/tpcc.c

**Overall:** TPC-C benchmark driver with unchecked `malloc`.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 60 | LOW (benchmark) | `malloc` return unchecked |

### benchmark/tpcc/tpcc_load.c

**Overall:** Data loading with multiple `sprintf` buffer overflow risks.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| 26 | LOW (benchmark) | `sprintf` potential buffer overflow in `random_zip` |
| 101 | LOW (benchmark) | `sprintf` without bounds checking |
| 101-122 | LOW (benchmark) | Multiple unchecked `sqlite3_prepare_v2` |

### benchmark/tpcc/tpcc_txn.c

**Overall:** Transaction execution with many unchecked `sqlite3_step` calls.

| Line(s) | Severity | Finding |
|----------|----------|---------|
| Various | LOW (benchmark) | ~20 unchecked `sqlite3_step` return values |

---

## Cross-Cutting Concerns

### 1. Crash Safety Analysis

The VFS uses a **download-modify-upload** pattern for the main database file. Committed transaction safety depends on:

1. **Journal mode (DELETE):** Journal uploaded via `xSync(journal)` → DB pages flushed via `xSync(db)` → Journal deleted via `xDelete`. This ordering is correct — if a crash occurs after journal upload but before DB flush, the journal exists for recovery. **However**, the multi-page flush in `xSync(db)` is **not atomic** — a crash during sequential writes leaves partially-updated pages. Azure Page Blob individual writes are atomic at 512 bytes, but cross-page consistency is not guaranteed.

2. **WAL mode:** The append blob model is a good fit. However, finding **H9** (duplicate data on partial append failure) is a real data integrity risk. A crash during `walNeedFullResync` can lose WAL data (finding **M12**).

3. **Lease expiry during flush:** The 30s/60s lease with 15s renewal is well-designed, but a long network stall exceeding the lease duration during `xSync` would cause the flush to silently succeed on the client side while the lease has been taken by another writer. The code renews every 50 iterations (line 896), but doesn't check the lease is still valid after each `page_blob_write`.

### 2. Authentication Security

- **Shared Key auth** is correctly implemented following Azure's StringToSign spec.
- **SAS tokens** are properly stripped of leading `?` and appended to URLs.
- **Credential storage:** Keys and SAS tokens are stored in fixed-size arrays in the client struct, scrubbed on destroy — but the scrubbing may be optimized away (C4).
- **No credential logging:** Debug timing output correctly doesn't include credentials. ✓

### 3. Error Propagation Completeness

Production code (`azqlite_vfs.c`, `azure_client.c`) checks almost all return codes. The main exceptions are:
- `lease_release` in `xUnlock` (intentionally ignored — documented)
- `lease_release` in `xClose` (intentionally ignored — logged)
- `curl_slist_append` (universally unchecked — H6)
- `BIO_write`/`BIO_flush` (unchecked — H2)

Test code has pervasive unchecked returns (~60+) which violates the project directive but is acceptable for test setup code.

### 4. Memory Management Discipline

Overall discipline is good:
- Every `malloc`/`realloc` in production code checks for NULL ✓
- `xClose` frees all buffers (aData, aDirty, aJrnlData, aWalData, zBlobName) ✓
- `xOpen` error paths free zBlobName and buffers before returning errors ✓
- `azure_client_destroy` cleans up curl handles and scrubs secrets ✓
- `azure_buffer_free` is called consistently after use ✓

The one weak area is that `xOpen` WAL error paths at line 1414 don't free `aWalData` if it was allocated by an earlier successful path, but this is mitigated by the `memset(p, 0, ...)` at line 1211 which ensures NULL pointers initially.

---

*End of review.*
