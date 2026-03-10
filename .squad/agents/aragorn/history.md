# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (azqlite) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

## Key Context

- VFS implementation via sqlite3_vfs — xOpen, xRead, xWrite, xSync, xLock, etc.
- Goal: no SQLite source modifications — use VFS extension API exclusively
- Open question: WAL mode vs Journal mode vs both?
- SQLite locking model must map correctly to blob operations
- License: MIT

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->

### VFS API Deep Dive (2026-03-10)

**Three-layer architecture:** `sqlite3_vfs` (open/delete/access) → `sqlite3_file` (base struct, first member is pMethods) → `sqlite3_io_methods` (read/write/lock/sync per-file).

**sqlite3_io_methods versions:** v1 = core I/O + locking (12 methods). v2 = adds xShmMap/xShmLock/xShmBarrier/xShmUnmap (WAL support). v3 = adds xFetch/xUnfetch (mmap). We need v1 only for MVP.

**Key method signatures:**
- `xRead(file, pBuf, iAmt, iOfst)` — MUST zero-fill on short read or corruption follows
- `xWrite(file, pBuf, iAmt, iOfst)` — writes capped at 128KB internally
- `xSync(file, flags)` — flags: SQLITE_SYNC_NORMAL(0x02) or SQLITE_SYNC_FULL(0x03), optionally | SQLITE_SYNC_DATAONLY(0x10)
- `xLock(file, level)` — level is SHARED(1)/RESERVED(2)/PENDING(3)/EXCLUSIVE(4), never NONE
- `xUnlock(file, level)` — level is SHARED(1) or NONE(0) only
- `xFileControl(file, op, pArg)` — return SQLITE_NOTFOUND for unknown opcodes

**Locking model (5 levels):** NONE→SHARED→RESERVED→PENDING→EXCLUSIVE. PENDING is never explicitly requested (internal transitional). Unix VFS uses POSIX fcntl byte-range locks at offset 0x40000000 (1GB). Azure has no shared lock primitive — major gap.

**WAL mode is NOT feasible for remote storage.** Requires shared memory (xShmMap) with sub-millisecond latency between processes. The nolockIoMethods sets xShmMap=0, which prevents WAL mode — we should do the same. Journal mode (DELETE/TRUNCATE) is the correct choice.

**File types:** xOpen receives type flags. MAIN_DB(0x100) and MAIN_JOURNAL(0x800) must be remote. TEMP_DB, TEMP_JOURNAL, TRANSIENT_DB, SUBJOURNAL should be local (delegate to default VFS).

**Page alignment:** Default page size 4096, range 512-65536, always multiples of 512. Azure Page Blobs require 512-byte alignment — fully compatible.

**Device characteristics for Azure:** `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`. Do NOT set ATOMIC — journal provides crash safety. PSOW eliminates journal padding I/O.

**Sector size:** Return 4096 (matches default page size and SQLITE_DEFAULT_SECTOR_SIZE).

**VFS registration:** `sqlite3_vfs_register(pVfs, makeDflt)`. Select via `sqlite3_open_v2(file, &db, flags, "azqlite")` or URI `?vfs=azqlite`.

**All VFS methods are synchronous.** Azure latency (5-50ms per op) means read cache is mandatory. Without cache, 100-page read = ~5 seconds.

**szOsFile:** SQLite allocates this many bytes for our sqlite3_file subclass. We fill it in during xOpen, we don't allocate it.

**pMethods trap:** If xOpen sets pMethods non-NULL, xClose WILL be called even on xOpen failure. Set pMethods=NULL to prevent xClose on failure.

**nolock pattern (simplest reference):** `nolockLock()` returns SQLITE_OK always. `nolockCheckReservedLock()` sets *pResOut=0. This is our MVP locking strategy.

### Design Review — MVP 1 Architecture (2026-03-10)

- **Full design review at `research/design-review.md`.** 11 decisions covering blob types, locking, caching, WAL, testing, build system, error handling, auth, naming, VFS registration, and MVP scope.
- **Gandalf corrected my nolock proposal:** Two-level lease-based locking from day 1 (SHARED=no lease, RESERVED+=acquire lease). Prior art shows deferred locking leads to corruption. Updated in D3 of decisions.md.
- **Key architecture: azure_ops_t vtable.** The swappable function pointer table between VFS and Azure client is both the production interface and the test seam. Defined in design-review.md Appendix A. This is the critical boundary between my VFS code and Frodo's Azure client code.
- **io_methods iVersion=1** — no WAL, no mmap. Eliminates 6 method implementations.
- **File type routing:** MAIN_DB and MAIN_JOURNAL → Azure. Everything else → default local VFS. Detected via flags in xOpen.
- **Journal handled as block blob** (sequential write/read pattern). DB as page blob (random R/W).
- **Write buffer with dirty page bitmap:** xWrite→memcpy+dirty bit, xSync→PUT Page per dirty page. Batches writes to sync time.
- **Full-blob cache from day 1:** Download entire blob on xOpen into malloc'd buffer. xRead=memcpy. Gandalf overrode MVP 2 deferral — my uncached analysis proved 5s for 100 pages is untestable.
- **Two-level lease locking:** SHARED requires no lease (reads always work). RESERVED/EXCLUSIVE acquire 30s blob lease. Release on unlock. xCheckReservedLock uses HEAD to detect held leases. Inline renewal (no background thread).
- **Error handling:** 409 (lease conflict) or 429 (throttle) → SQLITE_BUSY (retryable). All else after retry → SQLITE_IOERR_* (fatal). 5 retries, 500ms exponential backoff + jitter.
- **Device characteristics:** `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`. NOT ATOMIC — journal safety is needed.
- **VFS name "azqlite"**, non-default registration. Usage: `sqlite3_open_v2(name, &db, flags, "azqlite")`.
- **Source layout:** `src/` (azqlite_vfs.c, azure_client.c/.h, azure_auth.c, azure_error.c, azqlite.h), `test/` (test_main.c, test_vfs.c, mock_azure_ops.c, test_integration.c).
- **Top risks:** (1) Lease expiry during long transactions, (2) Partial sync failure — both mitigated by journal-first ordering and inline lease renewal.

### Cross-Agent Context: Key Interface Contract — azure_ops_t (2026-03-10)

- **Critical boundary with Frodo's Azure client.** Design review D5 mandated a swappable Azure operations interface (function pointer vtable `azure_ops_t`) for testability. Non-negotiable.
- **What I (Aragorn) need from azure_ops_t:**
  - `azure_blob_read()` — GET with Range header, returns buffer
  - `azure_blob_write()` — PUT Page with 512-byte alignment, max 4 MiB per call
  - `azure_blob_size()` — HEAD request, returns Content-Length
  - `azure_blob_truncate()` — Set Blob Properties with new length
  - `azure_lease_acquire()` — Acquire 30s lease, renew inline
  - `azure_lease_release()` — Release lease
  - `azure_lease_check()` — HEAD request, check if lease is held
- **Frodo will deliver** refactored azure_client exporting these as vtable members. PoC code in `research/azure-poc/` shows the patterns.
- **Samwise will provide** mock_azure_ops.c for layer 1 unit tests.
- **See design-review.md Appendix A** for full azure_ops_t contract definition.
