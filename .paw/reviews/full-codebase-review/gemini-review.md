# Gemini Review: Testing, Distributed Systems & Azure API

## Executive Summary
The **azqlite** project demonstrates a high level of distributed systems maturity and code quality. The core architecture—mapping SQLite VFS calls to Azure Blob Storage primitives (Page Blobs for DB, Block Blobs for Journal, Append Blobs for WAL)—is sound and follows established best practices for cloud-native storage.

The implementation of **Distributed Locking** (via Azure Leases) and **Crash Recovery** (via Journal/WAL persistence) is robust. The code explicitly handles network failures, throttling (429), and partial write scenarios.

 However, a **Critical Testing Gap** exists: the VFS logic unit tests in `test/test_vfs.c` are currently disabled by default due to a missing preprocessor definition, meaning the VFS layer's interaction with the mock backend is not being exercised in the standard test suite.

## Critical Findings

### 1. VFS Unit Tests Disabled by Default
*   **File**: `test/test_vfs.c`
*   **Severity**: **CRITICAL**
*   **Description**: The "Layer 1 VFS Integration Tests" (Sections 12+) in `test/test_vfs.c`, which verify the actual `azqlite_vfs.c` logic against the mock backend, are guarded by `#ifdef ENABLE_VFS_INTEGRATION`. This macro is **not defined** in `test/test_main.c` or the standard build configuration.
*   **Impact**: The core VFS logic (locking state machine, error mapping, open/close flows) is **not being unit tested** in the main test run. The existing tests in `test/test_vfs.c` (Sections 1-11) verify the *Mock* implementation, not the VFS itself.
*   **Recommendation**: Define `#define ENABLE_VFS_INTEGRATION` in `test/test_main.c` or the Makefile to ensure these tests run.

### 2. Batch Write Partial Failure Handling
*   **File**: `src/azure_client.c`:1200 (`az_page_blob_write_batch`)
*   **Severity**: **HIGH** (Verified as Correct, but high risk)
*   **Description**: The batch write implementation uses `curl_multi` to write pages in parallel. If *any* page fails after retries, the function returns an error.
*   **Analysis**: This is **CORRECT** for SQLite consistency. If `xSync` fails, SQLite will treat the transaction as failed and rollback.
*   **Risk**: Ensure that `azqliteSync` (VFS layer) propagates this error immediately. A partial write that reports success would corrupt the database. Current code correctly returns `SQLITE_IOERR_FSYNC`.

## High Findings

### 1. Journal Existence Cache Robustness
*   **File**: `src/azqlite_vfs.c`:163 (`azqliteVfsData`)
*   **Severity**: **HIGH**
*   **Description**: The `journalCacheState` optimization avoids HEAD requests by tracking creation/deletion locally.
*   **Risk**: While valid for the "Single Writer" model enforced by Exclusive Locking, if a process crashes and restarts, the cache resets to `-1` (Unknown), forcing a check. This is correct. However, if the lease expires and another writer takes over (breaking the single-writer assumption due to split-brain or bug), the cache could be stale.
*   **Recommendation**: Ensure `journalCacheState` is invalidated whenever the lease is lost or re-acquired. Currently, it seems tied to the VFS instance, not the lease epoch.

### 2. WAL Truncate Overhead
*   **File**: `src/azqlite_vfs.c`:546 (`azqliteTruncate`)
*   **Severity**: **HIGH**
*   **Description**: Truncating a WAL file involves deleting and re-creating the Azure Append Blob.
*   **Impact**: This is a heavyweight metadata operation. High-throughput workloads with frequent checkpoints might suffer latency spikes.
*   **Recommendation**: Acceptable for MVP. For future optimization, consider logical truncation (writing a "reset" record) if Azure cost/latency becomes an issue.

## Medium Findings

### 1. Hardcoded Azurite Path in Tests
*   **File**: `test/test_integration.c`:35
*   **Severity**: **MEDIUM**
*   **Description**: `AZURITE_ENDPOINT` is hardcoded to `http://127.0.0.1:10000`.
*   **Recommendation**: Allow overriding via environment variable (e.g., `AZURITE_URL`) to support CI environments where service containers might be on different hosts.

### 2. Header Case Sensitivity
*   **File**: `src/azure_client.c`:87 (`curl_header_cb`)
*   **Severity**: **MEDIUM**
*   **Description**: Uses `strncasecmp` for header parsing.
*   **Status**: **Correct**. HTTP/1.1 headers are case-insensitive. Good practice.

## File-by-File Analysis

### src/azqlite_vfs.c
*   **Locking**: Implements a 2-level lease model (SHARED=no lease, RESERVED=lease). Checks dirty page count to decide lease duration (30s vs 60s). **Verdict: Solid.**
*   **xSync**: Correctly handles WAL vs Journal vs MainDB. Calls `leaseRenewIfNeeded` during long flushes. **Verdict: Robust.**
*   **xRead/xWrite**: Pure memory operations against `aData` buffer. Simple and correct.

### src/azure_client.c
*   **Retries**: Implements exponential backoff with jitter and `Retry-After` support. **Verdict: Excellent.**
*   **Concurrency**: Uses `curl_multi` for batch writes. Reuses connection pool. **Verdict: High Performance.**
*   **Auth**: Supports both SAS and SharedKey. SAS takes precedence. **Verdict: Flexible.**

### test/test_vfs.c
*   **Mock Verification**: Thoroughly tests the *Mock* to ensure it behaves like Azure (alignment, errors).
*   **VFS Tests**: As noted, the actual VFS tests are guarded and likely disabled.

### test/test_integration.c
*   **Real Testing**: Validates end-to-end functionality against Azurite. Essential for verifying libcurl/network stack.

## Test Coverage Gap Analysis
*   **VFS Logic**: The gap in `test/test_vfs.c` means the state machine logic in `azqlite_vfs.c` is primarily tested via integration tests. Unit tests should be enabled to verify edge cases (e.g., specific error mappings) that are hard to reproduce in integration.
*   **Crash Recovery**: `test/test_integration.c` tests "Journal Round-Trip", but specific crash recovery scenarios (e.g., killing the process mid-sync and verifying recovery on next open) are not explicitly automated.

## Operational Readiness
*   **Logging**: `fprintf(stderr)` used for timing and critical errors. Acceptable for library, but consider a callback for application-level logging.
*   **Configuration**: Flexible (Env vars or struct).
*   **Dependencies**: `libcurl` and `openssl`. Standard and stable.

## Final Verdict
**azqlite** is a high-quality, well-architected solution. The distributed systems handling (leases, retries, fencing) is implemented with care. Fixing the **Critical Testing Gap** (enabling VFS unit tests) makes this ready for rigorous QA and production.
