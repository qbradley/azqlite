# Testing Strategy Research: Azure Blob-Backed SQLite VFS

**Author:** Samwise (QA)
**Date:** 2026-03-10
**Status:** Research Complete — Recommendation Included

---

## Executive Summary

I tested every idea against reality before putting it on paper. The recommendation is a **four-layer test pyramid** using in-process C mocks at the base, Azurite for integration, Toxiproxy for fault injection, and optional real Azure for final validation. This gives us speed where we need it (thousands of unit tests in seconds), fidelity where it matters (real HTTP against Azurite), and the ability to break things on purpose (network faults via Toxiproxy). No single tool does everything. I don't trust any of them alone.

---

## Table of Contents

1. [Approach 1: Azurite (Microsoft's Azure Storage Emulator)](#1-azurite)
2. [Approach 2: Other Emulators and Mocks](#2-other-emulators)
3. [Approach 3: Building Our Own Fake Azure Blob Service](#3-custom-fake)
4. [Approach 4: Hybrid Approach](#4-hybrid)
5. [Approach 5: Testing Without Azure Emulation](#5-no-emulation)
6. [Approach 6: SQLite's Own Test Patterns](#6-sqlite-patterns)
7. [What Other Projects Do](#7-other-projects)
8. [API Surface We Must Test](#8-api-surface)
9. [Recommended Test Architecture](#9-recommendation)
10. [Test Pyramid](#10-pyramid)
11. [Risk Assessment](#11-risks)

---

## 1. Azurite (Microsoft's Azure Storage Emulator) <a name="1-azurite"></a>

### What It Is

Azurite is Microsoft's official, actively maintained open-source emulator for Azure Blob, Queue, and Table storage. It is the **only** supported local emulator — the older Azure Storage Emulator is deprecated.

### Maturity and Version

| Property | Value |
|----------|-------|
| Latest version | v3.33.0–v3.35.0 (as of early 2026) |
| API versions supported | Up to 2025-11-05 |
| License | **MIT** (fully compatible with our MIT project) |
| GitHub | github.com/Azure/Azurite |
| Language | TypeScript/Node.js |
| Backing storage | SQLite (ironically) or in-memory |

### Supported Operations (Critical for Us)

| Operation | Supported? | Fidelity Notes |
|-----------|------------|----------------|
| **Page Blob Create** | ✅ Yes | 512-byte alignment enforced |
| **Put Page (write)** | ✅ Yes | 4 MiB max, 512-byte alignment |
| **Get Blob (range read)** | ✅ Yes | ⚠️ Known Range header inconsistency (issue #1682) — may return 206 instead of 416 for out-of-bounds reads |
| **Get Blob Properties** | ✅ Yes | Returns Content-Length, lease state, lease status |
| **Page Blob Resize** | ✅ Yes | 512-byte alignment |
| **Block Blob Upload** | ✅ Yes | Single Put Blob |
| **Block Blob Download** | ✅ Yes | Full download |
| **Delete Blob** | ✅ Yes | |
| **Lease Acquire** | ✅ Yes | ⚠️ Timing edge cases may differ |
| **Lease Renew** | ✅ Yes | |
| **Lease Release** | ✅ Yes | |
| **Lease Break** | ✅ Yes | ⚠️ Break period timing may not be exactly real |
| **Shared Key Auth** | ✅ Yes | HMAC-SHA256 supported |
| **SAS Token Auth** | ✅ Yes | |
| **OAuth** | ⚠️ Partial | Not all enterprise scenarios |

### Known Fidelity Gaps

These matter for us. I'll test them all.

1. **Range header inconsistency** — Azurite may return `206 Partial Content` where Azure returns `416 Requested Range Not Satisfiable` for out-of-bounds requests. Our VFS reads use Range headers extensively. **Mitigation:** Write specific tests for boundary reads and verify behavior against both Azurite and real Azure.

2. **Lease timing** — Azurite simulates lease acquire/renew/release/break but exact timing under load may differ. We use leases for SQLite locking. **Mitigation:** Test lease semantics (acquire, conflict, break) separately from lease timing. Use real Azure for timing-sensitive lease tests.

3. **URL format** — Azurite uses IP-style URLs (`http://127.0.0.1:10000/devstoreaccount1/container/blob`) not DNS-style. Our client code must handle both. **Mitigation:** Parameterize the base URL in our client.

4. **No geo-replication or consistency model** — Single-instance only. Not relevant for MVP 1-2 but matters for MVP 3-4 (multi-machine).

5. **`--loose` mode** — Azurite can relax validation with `--loose` and `--skipApiVersionCheck`, which can hide bugs. **Recommendation:** Always run tests in strict mode.

6. **Error codes** — Some error code strings may differ from real Azure (e.g., specific XML error bodies). Our `azure_parse_error_xml` code should be tested against both.

### How to Run It

```bash
# Option 1: npm (fast for local dev)
npm install -g azurite
azurite --silent --inMemoryPersistence --blobHost 0.0.0.0

# Option 2: Docker (best for CI)
docker run -d -p 10000:10000 mcr.microsoft.com/azure-storage/azurite \
  azurite-blob --blobHost 0.0.0.0 --inMemoryPersistence

# Default connection string (well-known, not a secret):
# AccountName=devstoreaccount1
# AccountKey=Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==
# BlobEndpoint=http://127.0.0.1:10000/devstoreaccount1
```

### Performance

- **In-memory mode** (`--inMemoryPersistence`): Fast. No disk I/O. Suitable for thousands of tests.
- **Startup time**: ~1-2 seconds.
- **Request latency**: Sub-millisecond for simple operations locally.
- **Parallelism**: Handles concurrent requests reasonably, but it's single-threaded Node.js. For high-parallelism tests, use separate containers per test suite.

### Error/Failure Simulation

**Azurite cannot simulate failures.** It has no built-in fault injection for:
- Network drops
- Slow responses
- Authentication failures (other than invalid credentials)
- Partial writes
- Server errors (500, 503)

This is a critical gap. We need fault injection for our VFS testing. See Toxiproxy below.

### Verdict on Azurite

**Use it.** It's the best tool for API compatibility testing with zero Azure costs. MIT licensed. Actively maintained. Supports all our operations. But **don't trust it alone** — the fidelity gaps mean we need supplementary testing for edge cases, failures, and timing.

---

## 2. Other Azure Storage Emulators/Mocks <a name="2-other-emulators"></a>

### Azure Storage Emulator (Deprecated)

- Microsoft's older emulator (Windows-only, used SQL Server LocalDB).
- **Officially deprecated** in favor of Azurite.
- Do not use. Not cross-platform, not maintained.

### Third-Party Mocks

| Tool | Language | Notes |
|------|----------|-------|
| Mockaco | C#/.NET | Generic HTTP mock server. Overkill; requires .NET runtime. |
| WireMock | Java/.NET | Powerful HTTP stubbing. Could mock Azure REST API but massive setup. |
| mitmproxy | Python | HTTP proxy for interception. Could modify Azurite responses for fault injection but complex. |

### C-Specific Mock Libraries

There are **no existing C mock libraries specifically for Azure Blob Storage**. The Azure SDK for C uses hand-written mocks of the HTTP transport layer. This is the right pattern for us — see Section 5.

### Generic HTTP Mock Server

Possible but not recommended as primary approach. The Azure Blob REST API has enough complexity (headers, XML bodies, lease semantics, 512-byte alignment) that a generic HTTP mock would drift from real behavior quickly.

### Verdict

No viable alternative to Azurite for HTTP-level emulation. For in-process C testing, we write our own mocks (Section 5).

---

## 3. Building Our Own Fake Azure Blob Service <a name="3-custom-fake"></a>

### What It Would Take

A minimal HTTP server implementing our subset of the Azure Blob REST API:

| Operation | Implementation Effort | Complexity |
|-----------|----------------------|------------|
| PUT Blob (page blob create) | Low | Parse headers, create in-memory page array |
| PUT Page (write) | Medium | Validate 512-byte alignment, range headers |
| GET Blob (range read) | Medium | Parse Range header, return bytes |
| HEAD (Get Properties) | Low | Return Content-Length, lease headers |
| PUT (Lease) | High | Full lease state machine (acquired/breaking/broken/available/expired) |
| DELETE Blob | Low | Remove from store |

### Estimated Code Size

| Language | Lines of Code | Time to Build |
|----------|--------------|---------------|
| Python (Flask) | ~400-600 | 2-3 days |
| C (with microhttpd) | ~800-1200 | 5-7 days |
| Node.js (Express) | ~300-500 | 1-2 days |

### Advantages

- **Perfect control over error injection** — return 500, 503, timeout at any point.
- **Deterministic latency simulation** — add configurable delays per operation.
- **Partial write simulation** — cut the connection mid-transfer.
- **Lease timing control** — make leases expire exactly when we want.
- **No external dependencies** — could be in-process for C tests.

### Disadvantages

- **Maintenance burden** — must track Azure REST API changes.
- **Risk of divergence** — our fake may accept requests real Azure rejects (or vice versa).
- **Lease state machine is complex** — getting it right is significant effort.
- **False confidence** — tests pass against our fake but fail against real Azure.
- **Duplicates Azurite work** — Azurite already handles the happy-path well.

### Verdict

**Don't build a full fake.** Build a thin **fault injection layer** instead. Use Toxiproxy between our code and Azurite for network faults, and use in-process C mocks for unit testing the VFS. The HTTP-level faking is not worth the maintenance cost when Azurite exists.

However, a minimal **in-process C mock** of the Azure client API (not the HTTP API) is essential — see Section 5.

---

## 4. Hybrid Approach <a name="4-hybrid"></a>

### The Proposed Hybrid

| Layer | Tool | Purpose |
|-------|------|---------|
| Unit tests | In-process C mocks | Fast, no HTTP, test VFS logic in isolation |
| Integration tests | Azurite | Real HTTP, API compatibility, happy path + basic error handling |
| Fault injection | Toxiproxy + Azurite | Network faults, latency, partial failures |
| Custom error injection | In-process C mocks | Specific Azure error codes, OOM, partial writes at exact points |
| Crash recovery | In-process C mocks + Azurite | Kill process at specific points, verify data |
| Final validation | Real Azure (CI only) | Verify fidelity, catch Azurite gaps |

### Why This Is Right

1. **Speed**: Unit tests with C mocks run in milliseconds. We can run thousands.
2. **Fidelity**: Azurite gives us real HTTP against a real-ish Azure API.
3. **Failure testing**: Toxiproxy lets us break the network deterministically.
4. **Safety net**: Real Azure in CI catches anything the emulator missed.
5. **No single point of trust**: I don't trust any one tool. Multiple layers catch different bugs.

### Verdict

**This is the recommended approach.** See Section 9 for the full architecture.

---

## 5. Testing Without Azure Emulation <a name="5-no-emulation"></a>

### In-Process Mock of Azure Client API

The key insight: **our VFS doesn't speak HTTP directly**. It calls Frodo's Azure client functions (`azure_page_blob_read`, `azure_page_blob_write`, etc.). We can mock at this boundary.

```
┌─────────────────────┐
│  SQLite Core         │
│  (sqlite3_vfs)       │
├─────────────────────┤
│  Our VFS Layer       │   ← Test this with mocks below
│  (Aragorn's code)    │
├─────────────────────┤
│  Azure Client API    │   ← Mock this interface
│  (Frodo's code)      │
├─────────────────────┤
│  libcurl / HTTP      │   ← Test with Azurite
└─────────────────────┘
```

### Mock Design

Define a **function pointer table** (vtable) for the Azure operations:

```c
typedef struct azure_ops {
    azure_err_t (*page_blob_create)(void *ctx, const char *name, int64_t size, azure_error_t *err);
    azure_err_t (*page_blob_write)(void *ctx, const char *name, int64_t offset, const uint8_t *data, size_t len, azure_error_t *err);
    azure_err_t (*page_blob_read)(void *ctx, const char *name, int64_t offset, size_t len, azure_buffer_t *out, azure_error_t *err);
    azure_err_t (*blob_get_properties)(void *ctx, const char *name, int64_t *size, char *lease_state, char *lease_status, azure_error_t *err);
    azure_err_t (*page_blob_resize)(void *ctx, const char *name, int64_t new_size, azure_error_t *err);
    azure_err_t (*blob_delete)(void *ctx, const char *name, azure_error_t *err);
    azure_err_t (*lease_acquire)(void *ctx, const char *name, int duration, char *lease_id, size_t lease_id_size, azure_error_t *err);
    azure_err_t (*lease_renew)(void *ctx, const char *name, const char *lease_id, azure_error_t *err);
    azure_err_t (*lease_release)(void *ctx, const char *name, const char *lease_id, azure_error_t *err);
    azure_err_t (*lease_break)(void *ctx, const char *name, int break_period, int *remaining, azure_error_t *err);
} azure_ops_t;
```

The VFS layer takes an `azure_ops_t*` and a context pointer. In production, it points to the real implementation. In tests, it points to a mock that:

- Stores pages in a `malloc`'d buffer (simulating a page blob).
- Tracks lease state in a simple state machine.
- Can be configured to **fail at specific call counts** (e.g., "fail the 3rd write with AZURE_ERR_TRANSIENT").
- Can be configured to **inject delays** (for timeout testing).
- Can be configured to **corrupt data** (for integrity testing).
- Runs **entirely in-process** — no HTTP, no external dependencies, sub-millisecond per call.

### What This Tests

- VFS xRead/xWrite correctly translate SQLite pages to Azure page blob operations
- VFS xLock/xUnlock correctly use Azure leases
- VFS handles Azure errors correctly (retry, give up, return SQLITE_IOERR)
- VFS handles partial reads (shorter response than requested)
- VFS handles resize during write
- VFS crash recovery (simulate crash by resetting mock state mid-transaction)

### What This Doesn't Test

- Actual HTTP serialization (headers, XML bodies)
- Actual Azure authentication (HMAC-SHA256 signing)
- Actual network behavior (latency, drops, retries)
- Azure API compatibility (status codes, error formats)

These gaps are covered by Azurite and real Azure testing.

### Verdict

**Essential foundation.** This is the base of our test pyramid. Every VFS test that doesn't need HTTP should use this.

---

## 6. SQLite's Own Test Patterns <a name="6-sqlite-patterns"></a>

I dug through `sqlite-autoconf-3520000/sqlite3.c` thoroughly. Here's what SQLite does that we should learn from:

### Test VFS Implementations in SQLite

| VFS | Location | Purpose |
|-----|----------|---------|
| **memdb** | Lines 54700-55670 | In-memory VFS for serialization testing. Stores entire DB as `malloc`'d buffer. |
| **kvvfs** | Lines 38579-39637 | Key-value VFS for embedded scenarios. Separate I/O methods for DB vs journal. |
| **unix-none** | Line 48150+ | Unix VFS variant with **no locking**. For testing without lock contention. |
| **unix-dotfile** | Line 48150+ | Dotfile-based locking variant. |

### Fault Injection Patterns

SQLite uses a **callback-based fault injection system**:

```c
// Install a fault callback via test control API
sqlite3_test_control(SQLITE_TESTCTRL_FAULT_INSTALL, my_fault_callback);

// Throughout the code, strategic fault injection points:
if (sqlite3FaultSim(400)) return SQLITE_IOERR;  // I/O error
if (sqlite3FaultSim(410)) return SQLITE_BUSY;    // Lock error
if (sqlite3FaultSim(650)) return SQLITE_NOMEM;   // Memory map error
```

**This is exactly what we should do.** Our Azure client mock should have numbered fault injection points that a test callback can trigger.

### System Call Override Pattern

SQLite's Unix VFS allows overriding 29 system calls via `xSetSystemCall()`:
- `open`, `close`, `read`, `write`, `pread`, `pwrite`, `fstat`, `ftruncate`, `fcntl`, `mmap`, etc.

Each call has a `pCurrent` and `pDefault` pointer. Tests can swap in a failing version and restore the default after. **We should do the same with our Azure operations.**

### sqlite3_io_methods Structure (What We Must Implement)

```c
struct sqlite3_io_methods {
    int iVersion;
    int (*xClose)(sqlite3_file*);
    int (*xRead)(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
    int (*xWrite)(sqlite3_file*, const void*, int iAmt, sqlite3_int64 iOfst);
    int (*xTruncate)(sqlite3_file*, sqlite3_int64 size);
    int (*xSync)(sqlite3_file*, int flags);
    int (*xFileSize)(sqlite3_file*, sqlite3_int64 *pSize);
    int (*xLock)(sqlite3_file*, int);
    int (*xUnlock)(sqlite3_file*, int);
    int (*xCheckReservedLock)(sqlite3_file*, int *pResOut);
    int (*xFileControl)(sqlite3_file*, int op, void *pArg);
    int (*xSectorSize)(sqlite3_file*);
    int (*xDeviceCharacteristics)(sqlite3_file*);
    // Version 2+: Shared memory (WAL mode)
    int (*xShmMap)(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
    int (*xShmLock)(sqlite3_file*, int offset, int n, int flags);
    void (*xShmBarrier)(sqlite3_file*);
    int (*xShmUnmap)(sqlite3_file*, int deleteFlag);
    // Version 3+: Memory mapping
    int (*xFetch)(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
    int (*xUnfetch)(sqlite3_file*, sqlite3_int64 iOfst, void *p);
};
```

Each of these needs testing. The memdb VFS is a good template — it implements all of these against an in-memory buffer.

### Key Takeaway from SQLite

**SQLite's testing philosophy: make the underlying layer swappable, then swap in failing versions.** Our architecture should follow this exactly: the Azure client layer is swappable, and our test mocks can fail in any way we program them to.

---

## 7. What Other Projects Do <a name="7-other-projects"></a>

### LiteFS (Fly.io)

- Passes the **full SQLite TCL test suite** — this is the gold standard.
- Uses FUSE to intercept file operations.
- Has a `mock/` directory with mock implementations for internal interfaces.
- Integration tests use Docker with real FUSE mounts.
- System tests exercise replication and failover.

**Lesson for us:** Aim to pass SQLite's own test suite through our VFS. If SQLite's tests pass, our VFS is correct.

### rqlite

- Go unit tests with mocked network/consensus/storage layers.
- Python-based end-to-end tests for multi-node clusters.
- Simulates network partitions, leader elections, failures.
- Each node uses temporary storage for test isolation.

**Lesson for us:** Test failure recovery end-to-end, not just unit-level.

### Azure SDK for C

- Uses hand-written mocks of the HTTP transport layer (no mocking framework).
- Interface abstraction: inject a mock transport that returns pre-defined responses.
- Uses Azurite for integration testing.
- Design guidelines emphasize testability through injectable dependencies.

**Lesson for us:** The vtable pattern for Azure operations is exactly what the Azure SDK team recommends for C.

---

## 8. API Surface We Must Test <a name="8-api-surface"></a>

From our PoC (`research/azure-poc/azure_blob.h`), these are the exact operations:

### Page Blob Operations (Critical Path)
| Function | Maps to VFS | Priority |
|----------|-------------|----------|
| `azure_page_blob_create()` | xOpen (create new DB) | P0 |
| `azure_page_blob_write()` | xWrite | P0 |
| `azure_page_blob_read()` | xRead | P0 |
| `azure_page_blob_resize()` | xTruncate / grow | P0 |
| `azure_blob_get_properties()` | xFileSize, xCheckReservedLock | P0 |
| `azure_blob_delete()` | xDelete | P1 |

### Lease Operations (Locking)
| Function | Maps to VFS | Priority |
|----------|-------------|----------|
| `azure_lease_acquire()` | xLock (SHARED→RESERVED→EXCLUSIVE) | P0 |
| `azure_lease_renew()` | Background during write transaction | P0 |
| `azure_lease_release()` | xUnlock | P0 |
| `azure_lease_break()` | Recovery from crashed writer | P1 |

### Block Blob Operations (Journal/WAL)
| Function | Maps to VFS | Priority |
|----------|-------------|----------|
| `azure_block_blob_upload()` | Journal/WAL file write | P1 |
| `azure_block_blob_download()` | Journal/WAL file read | P1 |

### Support Functions
| Function | Priority |
|----------|----------|
| `azure_auth_sign()` | P0 (must be correct or nothing works) |
| `azure_parse_error_xml()` | P0 |
| `azure_is_transient_error()` | P0 |
| `azure_retry_execute()` | P0 |

---

## 9. Recommended Test Architecture <a name="9-recommendation"></a>

### Layer 1: Unit Tests (In-Process C Mocks)

**Tool:** Custom C mock implementing `azure_ops_t` vtable
**Runs:** Every build, every commit. < 5 seconds for full suite.
**Tests:**

- VFS method correctness (every `sqlite3_io_methods` function)
- Error handling for every `azure_err_t` code
- Retry logic (mock returns transient errors N times, then succeeds)
- Lease state machine (acquire/renew/release/break/conflict/expire)
- Page alignment enforcement
- File size tracking through blob properties
- Lock escalation (NONE → SHARED → RESERVED → PENDING → EXCLUSIVE)
- Crash recovery: reset mock mid-transaction, verify rollback
- SQLite's own `sqlite3FaultSim`-style numbered fault points
- OOM handling (malloc failure at specific points)

**Estimated tests:** 200-400

### Layer 2: Integration Tests (Azurite)

**Tool:** Azurite in Docker (in-memory mode)
**Runs:** Every PR, every merge. < 60 seconds for full suite.
**Tests:**

- Real HTTP requests against Azure Blob REST API
- Authentication (HMAC-SHA256 signing correctness)
- XML error parsing from real Azure-format responses
- Page blob create → write → read → verify roundtrip
- Block blob upload → download → verify
- Lease acquire → conflict → break → re-acquire
- Range read boundary conditions
- Concurrent readers (multiple connections to same blob)
- End-to-end: `sqlite3_open → INSERT → SELECT → sqlite3_close` through our VFS against Azurite
- SQLite TCL test suite through our VFS (subset that makes sense)

**Estimated tests:** 50-100

### Layer 3: Fault Injection Tests (Toxiproxy + Azurite)

**Tool:** Toxiproxy between our code and Azurite
**Runs:** Nightly or per-release. < 5 minutes.
**Tests:**

- Network latency (500ms, 2s, 10s delays)
- Connection timeout (no response)
- Connection reset mid-transfer
- Bandwidth throttling (simulate slow network)
- Blackhole (total network outage)
- Partial response (connection drops after N bytes)

**How Toxiproxy works with our stack:**
```
Our VFS → libcurl → Toxiproxy (port 12345) → Azurite (port 10000)
                        ↑
                   Inject faults here
                   via HTTP API (port 8474)
```

Configure Toxiproxy from our C test harness using libcurl to hit its REST API, or use shell scripts.

**Estimated tests:** 20-40

### Layer 4: Real Azure Validation (CI Only)

**Tool:** Real Azure Storage account (test account)
**Runs:** Weekly CI job, or before releases. Cost: < $1/month.
**Tests:**

- All Layer 2 tests re-run against real Azure
- Lease timing validation (actual 15-60 second durations)
- Range header behavior (verify 416 vs 206)
- Error code format verification
- Performance baseline (actual network latency)

**Estimated tests:** Same as Layer 2 (50-100), just different backend

### Test Framework

**Recommended:** Custom C test harness following SQLite's own patterns.

Why not `check` or `cmocka`?
- We need tight integration with SQLite's test infrastructure.
- SQLite uses its own assertion macros and test runner.
- A simple custom harness (assert macros + test registration) gives us maximum control.
- We can integrate SQLite's `sqlite3FaultSim` mechanism directly.

Harness structure:
```c
// test_main.c
#define TEST(name) static void name(void)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { fail(__FILE__, __LINE__, #a, #b); } } while(0)
#define ASSERT_OK(rc) ASSERT_EQ(rc, SQLITE_OK)

TEST(test_vfs_read_write_roundtrip) {
    // Setup mock
    azure_mock_t mock;
    azure_mock_init(&mock);
    // ... test ...
    azure_mock_cleanup(&mock);
}

// Registration
test_entry_t tests[] = {
    {"vfs_read_write_roundtrip", test_vfs_read_write_roundtrip},
    {"vfs_lock_escalation", test_vfs_lock_escalation},
    // ...
    {NULL, NULL}
};
```

### CI Pipeline

```yaml
# GitHub Actions
test-unit:
  runs-on: ubuntu-latest
  steps:
    - make test-unit            # Layer 1: C mocks, < 5 sec

test-integration:
  runs-on: ubuntu-latest
  services:
    azurite:
      image: mcr.microsoft.com/azure-storage/azurite
      ports: ["10000:10000"]
      options: --health-cmd "curl -f http://localhost:10000/" --health-interval 5s
  steps:
    - make test-integration     # Layer 2: Azurite, < 60 sec

test-fault-injection:
  runs-on: ubuntu-latest
  services:
    azurite:
      image: mcr.microsoft.com/azure-storage/azurite
      ports: ["10000:10000"]
    toxiproxy:
      image: ghcr.io/shopify/toxiproxy:latest
      ports: ["8474:8474", "12345:12345"]
  steps:
    - make test-faults          # Layer 3: Toxiproxy, < 5 min

test-azure-real:                # Layer 4: Weekly schedule
  runs-on: ubuntu-latest
  if: github.event_name == 'schedule'
  env:
    AZURE_STORAGE_ACCOUNT: ${{ secrets.TEST_STORAGE_ACCOUNT }}
    AZURE_STORAGE_KEY: ${{ secrets.TEST_STORAGE_KEY }}
  steps:
    - make test-azure           # Same tests, real Azure
```

---

## 10. Test Pyramid <a name="10-pyramid"></a>

```
                    ┌─────────────────┐
                    │   Real Azure    │  ~50 tests, weekly
                    │   (CI only)     │  Catches emulator gaps
                    ├─────────────────┤
                 ┌──┤  Fault Inject   │  ~30 tests, nightly
                 │  │  (Toxiproxy)    │  Catches failure handling bugs
                 │  ├─────────────────┤
              ┌──┤  │  Integration    │  ~75 tests, every PR
              │  │  │  (Azurite)      │  Catches HTTP/API bugs
              │  │  ├─────────────────┤
           ┌──┤  │  │    Unit Tests   │  ~300 tests, every commit
           │  │  │  │  (C Mocks)      │  Catches logic bugs
           │  │  │  └─────────────────┘
           │  │  │
Speed ◄────┘  │  └────► Fidelity
              │
              └────────► Coverage
```

---

## 11. Risk Assessment <a name="11-risks"></a>

### Risks Mitigated by This Strategy

| Risk | Mitigation |
|------|-----------|
| Azurite differs from real Azure | Layer 4 (real Azure) catches gaps |
| Tests too slow to run frequently | Layer 1 (C mocks) runs in seconds |
| Can't test network failures | Layer 3 (Toxiproxy) covers this |
| Mock diverges from real implementation | vtable pattern means same interface, swappable backend |
| Crash recovery bugs | Layer 1 mock can reset state at exact points |
| Lease timing bugs | Layer 4 (real Azure) for timing, Layer 1 for semantics |

### Remaining Risks

| Risk | Severity | Notes |
|------|----------|-------|
| Toxiproxy can't simulate partial HTTP response bodies perfectly | Medium | May need custom proxy for exact Azure error format simulation |
| Azurite page blob sparse write semantics may differ | Low | Test explicitly, verify with real Azure |
| Multi-machine locking can't be tested locally | High | Needs real Azure or multiple Azurite instances (MVP 3-4 concern) |
| SQLite WAL mode may have VFS requirements we haven't discovered | Medium | Run SQLite test suite through our VFS early |

### Dependencies to Resolve

1. **Architecture decision needed:** Does VFS use page blobs, block blobs, or both? (Affects which tests to prioritize)
2. **Aragorn must design the azure_ops_t vtable** (or equivalent abstraction) into the VFS layer for testability.
3. **Frodo must confirm** that the PoC API surface in `azure_blob.h` is the final interface.

---

## Summary of Tools

| Tool | License | Role | Cost |
|------|---------|------|------|
| Custom C mock (azure_ops_t) | Our code (MIT) | Unit test foundation | $0 |
| Azurite | MIT | Integration testing | $0 |
| Toxiproxy | MIT | Fault injection | $0 |
| Docker | Apache 2.0 | Container runtime for CI | $0 |
| Real Azure Storage | N/A | Final validation | ~$1/month |

**Total cost for complete test infrastructure: ~$1/month** (only for the optional real Azure tests).

---

## Final Recommendation

Build the four-layer pyramid. Start with Layer 1 (C mocks) because:
1. It's needed first — Aragorn can't verify VFS correctness without it.
2. It requires the `azure_ops_t` vtable design, which drives the architecture.
3. It's the fastest to iterate on.
4. Every other layer builds on it.

Then add Layer 2 (Azurite) when we have end-to-end HTTP working.
Then add Layer 3 (Toxiproxy) when we're testing resilience.
Layer 4 (real Azure) comes last, in CI only.

**I will write a test for every failure mode. If someone says "that can't happen," I'll write a test to prove it can.**

— Samwise
