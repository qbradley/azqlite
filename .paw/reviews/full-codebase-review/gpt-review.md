# GPT Review: Architecture, Correctness & Performance
## Executive Summary
azqlite has a promising basic shape: the VFS keeps a whole-db cache in memory (`src/azqlite_vfs.c:117-158`), abstracts Azure behind `azure_ops_t` (`src/azure_client.h:147-271`), and its current unit/integration suites both pass (`Makefile:164-177`; validated with `make test-unit` and `make test-integration`). The biggest production blockers are architectural: the VFS misroutes some non-database paths to Azure, global singleton state makes the implementation unsafe for multiple databases/connections/registrations, and the production HTTP client treats blob names as raw URL path fragments. Benchmarks also contain real correctness/validity bugs, including undefined calls into the public VFS API.

## Critical Findings
### 1. `xDelete`/`xAccess` violate the SQLite VFS routing contract for non-Azure files
- `azqliteOpen()` explicitly delegates every non-`MAIN_DB`/`MAIN_JOURNAL`/`WAL` open to the default VFS (`src/azqlite_vfs.c:1213-1217`).
- But `azqliteDelete()` and `azqliteAccess()` do not perform the same classification; if Azure ops exist they send **every** pathname to Azure first (`src/azqlite_vfs.c:1488-1514`, `src/azqlite_vfs.c:1529-1581`).
- That means temp files, transient files, delegated journals, and other local-only artifacts can be probed/deleted against blob storage instead of the default VFS. For SQLite, `xOpen`, `xAccess`, and `xDelete` need to agree on which namespace a path belongs to; here they do not.
- The existing temp-file test does not catch this because it makes no assertions after the temp-table operations (`test/test_vfs.c:1709-1720`).
- Severity: **CRITICAL**

### 2. Registration and transport state are process-global and unsynchronized
- The VFS stores mutable process-wide state in a single static `g_vfsData` (`src/azqlite_vfs.c:1683-1755`). Each registration zeroes and rewrites it (`src/azqlite_vfs.c:1718-1747`).
- Open files keep back-pointers into that singleton (`src/azqlite_vfs.c:145`, `src/azqlite_vfs.c:1231`), so a second registration can change `ops`, `ops_ctx`, `client`, and journal-cache state underneath already-open handles.
- The production client also reuses a single `CURL *` stored in `client->curl_handle` (`src/azure_client_impl.h:60-66`, `src/azure_client.c:206`, `src/azure_client.c:289-401`) and a shared `CURLM *` (`src/azure_client.c:1024-1040`) with no locking anywhere in `src/` (searching `mutex` only finds comments, not synchronization).
- This is not safe for multi-connection or multi-thread use, despite the build enabling `SQLITE_THREADSAFE=1` in `Makefile:22-24`.
- Severity: **CRITICAL**

## High Findings
### 1. Production URLs use raw blob names with no URI escaping
- `build_blob_url()` interpolates `blob_name` directly into the request URL (`src/azure_client.c:165-179`). There is no percent-encoding step before issuing HTTP requests.
- The codebase and tests assume names can contain path separators and special characters (`test/test_vfs.c:1176-1192`, `test/test_azure_client.c:712-780`), but those tests run only against the mock layer, not the libcurl path construction.
- In production, characters such as space, `#`, `?`, `%`, and some UTF-8 sequences will either generate invalid URLs or change request semantics.
- Severity: **HIGH**

### 2. WAL is accepted based on append ops alone, not on the actual shm/exclusive-mode precondition
- `azqliteFileControl()` approves `PRAGMA journal_mode=WAL` whenever append-blob ops are present (`src/azqlite_vfs.c:1094-1114`).
- The implementation still hard-fails any shared-memory map with `SQLITE_IOERR` (`src/azqlite_vfs.c:1167-1173`) and relies on the caller to have already executed `PRAGMA locking_mode=EXCLUSIVE`.
- The tests normalize this by always setting EXCLUSIVE first in WAL helpers (`test/test_wal.c:144-171`, `test/test_wal.c:231-243`), so the failure mode is masked.
- As written, the VFS can acknowledge WAL mode and only fail later when SQLite touches the shm API. That is a contract/usability bug in the exposed API surface.
- Severity: **HIGH**

### 3. Benchmark binaries call `azqlite_vfs_register()` with the wrong signature
- The public API is `int azqlite_vfs_register(int makeDefault);` (`src/azqlite.h:71-89`).
- `benchmark/speedtest1_wrapper.c` declares it as `extern int azqlite_vfs_register(void);` and calls it with no argument (`benchmark/speedtest1_wrapper.c:8-19`).
- `benchmark/tpcc/tpcc.c` repeats the same mistake (`benchmark/tpcc/tpcc.c:177-180`).
- That is undefined behavior in C: the benchmarks are not reliably requesting a known `makeDefault` value.
- Severity: **HIGH**

### 4. `azure_auth_sign_request()` has a fixed 4 KiB signing buffer with unchecked pointer advancement
- The function accumulates canonicalized headers/resource/query into `char string_to_sign[4096]` (`src/azure_auth.c:163-165`).
- It repeatedly does `p += snprintf(...)` without checking whether the returned size exceeded the remaining space (`src/azure_auth.c:176-192`, `src/azure_auth.c:233-261`).
- Once `snprintf()` truncates, `p` can advance beyond `end`; later `(size_t)(end - p)` becomes invalid and subsequent writes operate on undefined state.
- Blob names, container names, or long SAS/query strings are enough to stress this path.
- Severity: **HIGH**

### 5. Journal-existence caching is global, not per database
- The cache tracks exactly one journal blob name and one state in `azqliteVfsData` (`src/azqlite_vfs.c:170-177`).
- Journal open/access/delete all read and overwrite that singleton (`src/azqlite_vfs.c:1353-1369`, `src/azqlite_vfs.c:1495-1508`, `src/azqlite_vfs.c:1531-1564`).
- Two databases using the same registered VFS instance can poison each other’s journal cache state.
- Even without threading, this is a design flaw once the process uses more than one database blob.
- Severity: **HIGH**

## Medium Findings
### 1. `sqlite3_mprintf()` allocations are released with `free()` instead of `sqlite3_free()`
- `zBlobName` is allocated with `sqlite3_mprintf()` (`src/azqlite_vfs.c:1225`).
- It is later released with `free()` on both error paths and normal close (`src/azqlite_vfs.c:430`, `src/azqlite_vfs.c:1251-1463`).
- That is only accidentally safe when SQLite uses the system allocator; it breaks SQLite’s allocator contract once a custom allocator is installed.
- Severity: **MEDIUM**

### 2. ETag plumbing is effectively non-functional
- The VFS tracks ETags (`src/azqlite_vfs.c:138-140`) and tries to copy them from Azure responses on open and sync (`src/azqlite_vfs.c:1269-1272`, `src/azqlite_vfs.c:883-885`, `src/azqlite_vfs.c:933-935`).
- But the production header parser never captures the `ETag` header; it only records lease state/status, request id, error code, content length, lease time, and retry-after (`src/azure_client.c:83-155`).
- So the ETag-based cache-validation scaffolding is currently dead and cannot support the design notes in the VFS.
- Severity: **MEDIUM**

### 3. `benchmark/benchmark.c` treats exit status 1 as a successful run
- `run_command()` records success when the child exits with `0` **or `1`** (`benchmark/benchmark.c:40-49`).
- That masks benchmark failures and contaminates comparison output.
- Severity: **MEDIUM**

### 4. TPC-C loader/transactions silently ignore many SQLite errors
- In `tpcc_txn.c`, large portions of New Order, Payment, and Order Status only check `sqlite3_prepare_v2()` opportunistically and then ignore `sqlite3_step()` results (`benchmark/tpcc/tpcc_txn.c:71-149`, `benchmark/tpcc/tpcc_txn.c:185-229`, `benchmark/tpcc/tpcc_txn.c:292-323`).
- `tpcc_load.c` does the same for a large number of prepare/step calls during warehouse loading (`benchmark/tpcc/tpcc_load.c:98-158`, `benchmark/tpcc/tpcc_load.c:183-299`).
- The benchmark can therefore mark a transaction/load as successful even after individual SQL statements failed.
- Severity: **MEDIUM**

### 5. The implemented TPC-C mix does not match the encoded/claimed mix
- The schema header still encodes a five-way TPC-C split (`benchmark/tpcc/tpcc_schema.h:148-153`).
- The driver actually executes only three transaction types and routes the remaining 12% of the probability space into Order Status (`benchmark/tpcc/tpcc.c:343-372`).
- README text documents the simplification later (`benchmark/tpcc/README.md:207-213`) but earlier sections and constants still imply a more faithful mix (`benchmark/tpcc/README.md:7-12`, `benchmark/tpcc/README.md:156-160`).
- Severity: **MEDIUM**

### 6. Important auth/XML tests are compiled out of the default unit run
- The auth/XML tests are gated under `#ifdef ENABLE_AZURE_CLIENT_TESTS` (`test/test_azure_client.c:394-524`) and only conditionally run in the suite runner (`test/test_azure_client.c:1182-1195`).
- The default `test-unit` target does not define that macro (`Makefile:164-170` runs the default unit suite, and the `test_main` compile line at `Makefile:169-170` defines VFS/WAL coverage but not `ENABLE_AZURE_CLIENT_TESTS`).
- That leaves significant parts of the production auth/error-parsing path with no default unit coverage.
- Severity: **MEDIUM**

## Low Findings
### 1. Temp-file routing coverage is effectively a no-op
- `test_vfs_temp_files_use_local_storage` sets up temp-table activity but performs no assertions about `xAccess`, `xDelete`, or default-VFS-only behavior (`test/test_vfs.c:1709-1720`).
- That helps explain why the critical routing bug above survives a green unit suite.
- Severity: **LOW**

### 2. Integration/test documentation is stale relative to the current tree
- `test/README-INTEGRATION.md` still says integration tests fail because of Azurite auth issues (`test/README-INTEGRATION.md:62-69`), but the current integration suite passes.
- `test/TEST-MATRIX.md` also reports outdated test totals and stale file sizes (`test/TEST-MATRIX.md:7`, `test/TEST-MATRIX.md:24-25`, `test/TEST-MATRIX.md:139-140`).
- Severity: **LOW**

## File-by-File Analysis
### src/azqlite_vfs.c
- Core VFS implementation: full-db buffer for `MAIN_DB`, in-memory journal buffer for `MAIN_JOURNAL`, and append-blob WAL buffer (`src/azqlite_vfs.c:109-158`).
- `xSync()` coalesces dirty pages and optionally batch-writes them (`src/azqlite_vfs.c:627-676`, `src/azqlite_vfs.c:688-939`).
- Key issues: non-Azure path misrouting in `xDelete`/`xAccess` (`src/azqlite_vfs.c:1488-1581`), global journal cache (`src/azqlite_vfs.c:170-177`, `src/azqlite_vfs.c:1353-1369`), allocator mismatch for `zBlobName` (`src/azqlite_vfs.c:1225`, `src/azqlite_vfs.c:430`), and WAL approval that does not enforce the exclusive-lock prerequisite (`src/azqlite_vfs.c:1094-1114`, `src/azqlite_vfs.c:1167-1173`).

### src/azure_client.c
- Production REST client built around one reusable easy handle and one reusable multi handle (`src/azure_client.c:192-533`, `src/azure_client.c:1024-1460`).
- It correctly covers page blobs, block blobs, leases, batch page writes, and append blobs (`src/azure_client.c:544-1583`).
- Key issues: raw blob names in URLs (`src/azure_client.c:165-179`), unsynchronized shared libcurl state (`src/azure_client.c:206`, `src/azure_client.c:289-401`, `src/azure_client.c:1024-1040`), and missing ETag capture despite VFS expectations (`src/azure_client.c:83-155`).

### src/azure_client.h
- Clean abstraction boundary: `azure_err_t`, `azure_error_t`, `azure_buffer_t`, `azure_page_range_t`, and the `azure_ops_t` vtable are all centralized here (`src/azure_client.h:29-271`).
- The interface is sound, but the public contract does not currently express buffer lengths for lease-state/status outputs, which is why both mock and production code rely on fixed 32-byte caller buffers (`src/azure_client.h:188-194`).

### src/azure_client_impl.h
- Internal constants and concrete `azure_client_t` layout (`src/azure_client_impl.h:27-66`).
- Important architectural note: comments claim SQLite’s btree mutex serializes batch writes (`src/azure_client_impl.h:61-66`), but no code-level synchronization backs the single shared handles.

### src/azure_client_stub.c
- Stub vtable returns `AZURE_ERR_UNKNOWN` from all operations (`src/azure_client_stub.c:17-198`).
- Useful for building without libcurl/OpenSSL, but it means any benchmark or shell accidentally linked against the stub will fail at runtime rather than at registration time (`src/azure_client_stub.c:202-226`).

### src/azure_auth.c
- Implements base64, HMAC-SHA256, RFC1123 formatting, and Shared Key request signing (`src/azure_auth.c:30-157`).
- Main issue is the fixed-size `string_to_sign` builder with unchecked pointer growth (`src/azure_auth.c:163-263`).

### src/azure_error.c
- Straightforward HTTP/XML classification and retry-delay helpers (`src/azure_error.c:24-203`).
- The implementation is conservative and readable; no major code-level defect stood out here beyond its dependency on callers to preserve accurate HTTP/header details.

### src/azqlite.h
- Public registration API and config struct (`src/azqlite.h:42-89`).
- This header is correct; the benchmark wrappers are the code that misdeclare it.

### src/azqlite_shell.c
- Shell wrapper that registers azqlite as the default VFS before jumping into SQLite’s shell (`src/azqlite_shell.c:37-58`).
- The wrapper is minimal and correct for its intended CLI use.

### test/test_vfs.c
- Broad unit coverage for VFS registration, CRUD, locking, rollback journal behavior, and selected error cases (`test/test_vfs.c:1343-2316`).
- The notable gap is temp-file routing: the only dedicated test does not assert anything (`test/test_vfs.c:1709-1720`), so the `xDelete`/`xAccess` bug goes unnoticed.

### test/test_wal.c
- WAL tests are well-structured and intentionally set `locking_mode=EXCLUSIVE` before enabling WAL (`test/test_wal.c:144-171`, `test/test_wal.c:215-270`).
- That makes them good happy-path validation, but it also means they never pressure-test the non-exclusive failure path that production callers can reach.

### test/test_coalesce.c
- Focused verification of dirty-range coalescing, 4 MiB splitting, alignment, and batch/sequential sync behavior (`test/test_coalesce.c:308-349`, `test/test_coalesce.c:419-452`, `test/test_coalesce.c:528-840`).
- Good coverage for the coalescer itself.

### test/test_integration.c
- Real-Azurite coverage for page blobs, block blobs, leases, and end-to-end VFS roundtrips (`test/test_integration.c:53-85`, `test/test_integration.c:109-220`, `test/test_integration.c:312-519`).
- Useful validation that the production client works against Azurite today.

### test/test_azure_client.c
- Good mock-based coverage for enum stability, buffer helpers, retry categorization, and general mock vtable behavior (`test/test_azure_client.c:40-390`, `test/test_azure_client.c:534-1132`).
- Auth/XML tests exist but are compiled out by default (`test/test_azure_client.c:394-524`, `test/test_azure_client.c:1182-1195`).

### test/mock_azure_ops.c
- Complete in-memory implementation of page blobs, block blobs, leases, and append blobs for tests (`test/mock_azure_ops.c:240-807`).
- `mock_blob_get_properties()` writes lease strings with `strcpy()` (`test/mock_azure_ops.c:497-509`); safe with current callers, but it reinforces how strongly the test layer assumes fixed caller buffer sizes.

### test/mock_azure_ops.h
- Declares the test-only inspection/failure-injection surface (`test/mock_azure_ops.h:46-194`).
- Helpful seam for deterministic failure testing.

### test/test_harness.h
- Small `setjmp`-based harness with assertion macros (`test/test_harness.h:31-223`).
- Works for this project’s style; no major architectural concerns.

### test/test_main.c
- Single-translation-unit test runner for unit suites (`test/test_main.c:19-46`).
- Purposefully includes suite sources directly to share static harness state.

### test/run-integration.sh
- Starts Azurite, waits for readiness, runs the integration binary, and tears down cleanly (`test/run-integration.sh:42-146`).
- Script behavior matched reality during validation.

### test/test-azurite-connection.sh
- Simple smoke script that starts Azurite, curls the endpoint, and kills the process (`test/test-azurite-connection.sh:1-20`).
- Useful only as a manual diagnostic.

### test/README-INTEGRATION.md
- Describes integration-test workflow and Azurite credentials (`test/README-INTEGRATION.md:1-60`).
- The “Known Issues” section is stale now that the suite passes (`test/README-INTEGRATION.md:62-69`).

### test/TEST-MATRIX.md
- Detailed manual coverage ledger for VFS/client/integration cases (`test/TEST-MATRIX.md:22-240`).
- The matrix is out of date on totals and some file lengths (`test/TEST-MATRIX.md:7`, `test/TEST-MATRIX.md:24-25`, `test/TEST-MATRIX.md:139-140`).

### test/SECURITY-TESTING.md
- Security-testing playbook covering sanitizers, static analysis, fuzzing, and CI guidance (`test/SECURITY-TESTING.md:18-240`).
- Helpful operational documentation; no code-level issues here.

### benchmark/benchmark.c
- Shells out to compare local `speedtest1` vs azqlite-backed `speedtest1-azure` (`benchmark/benchmark.c:36-52`, `benchmark/benchmark.c:216-289`).
- Main defect is that exit code `1` is treated as success (`benchmark/benchmark.c:45`).

### benchmark/speedtest1.c
- Imported SQLite benchmark driver; azqlite-specific behavior comes from its existing `--vfs`, `--exclusive`, and `--journal` options (`benchmark/speedtest1.c:35-41`, `benchmark/speedtest1.c:77`, `benchmark/speedtest1.c:3239-3318`).
- I did not find azqlite-specific edits in this file.

### benchmark/speedtest1_wrapper.c
- Registers azqlite and then runs the imported benchmark as the default VFS (`benchmark/speedtest1_wrapper.c:8-33`).
- The incorrect `azqlite_vfs_register` prototype/call is a real correctness bug (`benchmark/speedtest1_wrapper.c:8-19`).

### benchmark/README.md
- Describes speedtest-based comparison harness, environment variables, and expected slowdown ranges (`benchmark/README.md:45-179`).
- Documentation is generally consistent with the harness, but it understandably cannot compensate for `benchmark.c`’s exit-status bug.

### benchmark/Makefile
- Builds local and production speedtest wrappers (`benchmark/Makefile:77-165`).
- The use of `-w` when compiling the speedtest wrappers suppresses exactly the kind of prototype mismatch warning that would have exposed the bad `azqlite_vfs_register()` declarations (`benchmark/Makefile:86-97`).

### benchmark/tpcc/tpcc.c
- Benchmark driver: parses args, opens the DB, optionally enables WAL, loads data, then runs the OLTP loop (`benchmark/tpcc/tpcc.c:150-465`).
- Key issues are the wrong `azqlite_vfs_register` call (`benchmark/tpcc/tpcc.c:177-180`) and the mismatch between encoded transaction weights and implemented mix (`benchmark/tpcc/tpcc.c:343-372`).

### benchmark/tpcc/tpcc_load.c
- Bulk loader for items, warehouse data, districts, customers, stock, orders, and order lines (`benchmark/tpcc/tpcc_load.c:38-341`).
- Several prepare/step results are unchecked, so data-load “success” can be overstated (`benchmark/tpcc/tpcc_load.c:98-158`, `benchmark/tpcc/tpcc_load.c:183-299`).

### benchmark/tpcc/tpcc_txn.c
- Implements New Order, Payment, and Order Status (`benchmark/tpcc/tpcc_txn.c:28-334`).
- Multiple prepares/steps are ignored, so failed SQL sub-operations do not necessarily fail the transaction result (`benchmark/tpcc/tpcc_txn.c:71-149`, `benchmark/tpcc/tpcc_txn.c:185-229`, `benchmark/tpcc/tpcc_txn.c:292-323`).

### benchmark/tpcc/tpcc_schema.h
- Defines schema DDL, scale constants, and nominal TPC-C mix percentages (`benchmark/tpcc/tpcc_schema.h:13-153`).
- The transaction-mix constants no longer match what `tpcc.c` actually executes.

### benchmark/tpcc/README.md
- Good high-level explanation of the custom TPC-C-style benchmark and its limitations (`benchmark/tpcc/README.md:74-213`).
- Early sections still imply a more faithful mix than the implementation delivers (`benchmark/tpcc/README.md:7-12`, `benchmark/tpcc/README.md:156-160`).

### benchmark/tpcc/Makefile
- Builds local and Azure TPC-C binaries (`benchmark/tpcc/Makefile:77-164`).
- Like the speedtest Makefile, it does not surface the public-API prototype mismatch in benchmark code.

## Cross-Cutting Concerns
- **Namespace consistency is the main VFS risk.** `xOpen()` knows some files are local while `xAccess()`/`xDelete()` do not (`src/azqlite_vfs.c:1213-1217`, `src/azqlite_vfs.c:1488-1581`). That is the most direct SQLite-contract defect in the tree.
- **The code is architected like a single-database/single-process prototype, not a production multi-connection VFS.** Static VFS state, one journal cache, and one shared client/easy handle all point the same way (`src/azqlite_vfs.c:163-177`, `src/azqlite_vfs.c:1683-1755`, `src/azure_client.c:206-401`, `src/azure_client.c:1024-1040`).
- **Mock coverage is materially stronger than production-path coverage.** Special-character blob names, auth helpers, and many error-classification paths are either mock-only or gated out of default runs (`test/test_vfs.c:1185-1192`, `test/test_azure_client.c:394-524`, `test/test_azure_client.c:1182-1195`).
- **Benchmark numbers should be treated carefully until the harness bugs are fixed.** The speedtest harness masks failures (`benchmark/benchmark.c:45`), and the TPC-C driver is not a faithful or fully checked transaction implementation (`benchmark/tpcc/tpcc.c:343-372`, `benchmark/tpcc/tpcc_txn.c:71-149`).
- **Validation performed:** `make test-unit`, `make test-integration`, `make -C benchmark all-production`, and `make -C benchmark/tpcc all-production` all completed successfully in this review session.
