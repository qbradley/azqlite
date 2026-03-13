# Parallel Read Fan-Out — Feasibility Analysis

**Author:** Gandalf 🏗️  
**Status:** Draft  
**Date:** 2025-03-13

## Problem

SQLite calls xRead one page at a time, synchronously. Each cache miss
triggers a blocking HTTP GET (~22ms). For random workloads like TPC-C,
a single transaction may need 10-30 pages scattered across the file.
These pages are fetched sequentially: 10 × 22ms = 220ms of pure
network latency, even though the pages are independent and could be
fetched in parallel.

## Can We Know Which Pages Will Be Needed?

**Short answer: No, not from the SQLite VFS API.** SQLite has no batch
read hint, no prefetch FCNTL, and no mechanism to tell the VFS what
pages are coming next. xRead is called synchronously, one page at a
time, and SQLite blocks until it returns.

However, there are several creative approaches:

## Approach 1: Deferred Read + Batch Flush (VFS-Level)

**Idea:** Return a "pending" page from xRead that's filled with zeros
or stale data, collect page requests, then flush them all in parallel
before the data is actually needed.

**Problem:** This violates the xRead contract. SQLite expects the
buffer to be populated when xRead returns SQLITE_OK. There's no async
callback mechanism. **Not feasible.**

## Approach 2: Background Prefetch Thread

**Idea:** Run a background thread that speculatively fetches pages the
VFS predicts will be needed, based on the access pattern. The main
thread's xRead checks the cache first (now populated by the background
thread).

**How it works:**
1. On every cache miss in xRead, record the page number
2. Background thread analyzes recent miss patterns
3. Thread issues parallel GETs (via curl_multi) for predicted pages
4. Main thread continues — next xRead may find the page already cached

**Feasibility:** High. The cache is shared between threads with a
mutex. curl_multi runs in the background thread. The main thread only
blocks if the predicted page hasn't arrived yet.

**Limitation:** The background thread is always one step behind. For
truly random access, there's nothing useful to predict. For sequential
scans, the existing single-GET readahead is already optimal.

**Best for:** Workloads with some locality but not pure sequential —
e.g., index traversals where the next level's pages can be predicted.

## Approach 3: Read-Ahead via SQLite's B-Tree Cursor Hints

**Idea:** SQLite has `BTREE_HINT_RANGE` — an advisory hint to custom
storage engines about upcoming key ranges. This is NOT exposed at the
VFS level, but we could intercept it via a custom B-tree layer.

**Problem:** Requires modifying SQLite internals or building a custom
virtual table. Too invasive for a VFS. **Not practical.**

## Approach 4: Batch Miss Collection in xRead (★ Recommended)

**Idea:** The most impactful approach doesn't try to predict — it
observes that SQLite's pager reads pages in bursts. When SQLite
processes a query, it typically reads several pages in quick
succession (e.g., walking a B-tree from root to leaf). We can exploit
this with a "collect and flush" pattern.

**How it works:**

### Phase 1: Miss Queue

When xRead has a cache miss:
1. Don't fetch immediately
2. Add the page number to a per-file "miss queue"
3. Return the page from a "zero page" (or stale placeholder)

Wait — this still violates the xRead contract. Instead:

### Phase 1 (revised): Speculative Multi-Range GET

When xRead has a cache miss for page P:
1. Before fetching P, look at the B-tree structure of page P itself:
   - If P is an interior page, its child pointers are known after
     we read P. We can immediately prefetch all child pages.
   - If P is a leaf page, the next leaf pointer is in the page.
2. Fetch P + predicted pages in a single multi-range GET or parallel
   GETs via curl_multi.
3. Cache all fetched pages.

**This is "read one, prefetch children" — a tree-aware readahead.**

**Feasibility:** Medium-high. Requires parsing SQLite B-tree page
format (well-documented, stable). After downloading an interior page,
we know exactly which child pages exist and can prefetch them in
parallel.

### Phase 2: Parallel Range GETs

Create `page_blob_read_parallel()` using the existing curl_multi
infrastructure from `page_blob_write_batch`:

```c
typedef struct {
    int64_t offset;
    size_t len;
    azure_buffer_t *out;
} azure_read_range_t;

azure_err_t page_blob_read_parallel(
    void *ctx, const char *name,
    const azure_read_range_t *ranges, int nRanges,
    azure_error_t *err);
```

This issues N GET requests in parallel via curl_multi. All N complete
in roughly the time of 1 (assuming sufficient bandwidth and Azure
can handle the parallelism — it can, up to ~30 concurrent requests).

## Approach 5: xFetch / Memory-Mapped Mode

**Idea:** Implement xFetch/xUnfetch (io_methods v3) to enable SQLite's
memory-mapped I/O path. When mmap is enabled, SQLite accesses pages
via pointer dereference instead of xRead calls.

**How it works for a network VFS:**
1. At open time, allocate a large mmap-like buffer
2. xFetch returns a pointer into this buffer
3. On first access (page fault or explicit check), fetch from Azure
4. Use `madvise` or custom logic for background prefetching

**Problem:** We'd need to simulate mmap for a network-backed file.
This is essentially building a virtual memory system. Complex but
powerful. Linux's FUSE does this. **Feasible but high complexity.**

## Approach 6: Query-Level Batching via sqlite3_exec Hooks

**Idea:** Use `sqlite3_progress_handler` or `sqlite3_trace_v2` to
detect when a new SQL statement begins executing. At that moment,
analyze the query plan (EXPLAIN) to determine which tables/indexes
will be accessed, then prefetch their root and interior pages.

**How it works:**
1. Register a trace callback for `SQLITE_TRACE_STMT`
2. When a statement starts, run EXPLAIN on it
3. Extract table/index names from the plan
4. Prefetch root pages and first-level interior pages for each
5. These pages are in cache by the time xRead is called

**Feasibility:** Medium. Works at the application level (not VFS), so
it requires cooperation from the application code. Good for benchmarks
where we control the SQL. Not transparent.

## Recommended Implementation Plan

### Priority 1: `page_blob_read_parallel` (Infrastructure)

Add a curl_multi-based parallel read to azure_client.c, mirroring the
existing `page_blob_write_batch` pattern. This is pure infrastructure
that every other approach needs.

```c
/* In azure_ops: */
azure_err_t (*page_blob_read_parallel)(
    void *ctx, const char *name,
    const azure_read_range_t *ranges, int nRanges,
    azure_error_t *err);
```

### Priority 2: Tree-Aware Prefetch in xRead

When xRead fetches an interior B-tree page (type 0x02 or 0x05):
1. Parse child page pointers from the fetched page
2. Issue parallel GETs for all child pages not already in cache
3. Insert children into cache

This turns a B-tree traversal from depth × 22ms into ~22ms total
(one round-trip for the root, children arrive in parallel).

### Priority 3: Background Prefetch Thread

For sustained workloads, a background thread that pre-warms the cache
based on observed access patterns. Lower priority because `prefetch=all`
solves the warmup problem for databases that fit in cache.

## Performance Model

| Scenario | Current | With Parallel Reads |
|----------|---------|-------------------|
| B-tree lookup (3 levels) | 3 × 22ms = 66ms | 22ms + 22ms = 44ms (root, then children in parallel) |
| 10 random pages | 10 × 22ms = 220ms | ~22ms (all 10 in parallel) |
| Sequential scan (1000 pages) | 1 × 22ms (single GET) | Same (already optimal) |
| TPC-C New Order (15 items) | ~15 × 22ms = 330ms | ~22-44ms (parallel item lookups) |

The parallel read approach could improve random-access TPC-C throughput
by **5-10×** for cold-cache scenarios.

## Open Questions

1. Azure Page Blob supports single-range GET requests. Does it support
   multi-range GETs (HTTP Range header with multiple byte ranges)?
   If yes, we could fetch N non-contiguous ranges in a single request.
   If no, we need N parallel requests (still fast via curl_multi).

2. What's Azure's practical concurrency limit per blob? Testing needed
   to find the sweet spot (likely 16-32 parallel GETs).

3. For tree-aware prefetch, should we prefetch ALL children of an
   interior page, or just the ones likely to be accessed? For point
   lookups, only one child is accessed. For range scans, many children
   are accessed. The access pattern determines the right strategy.
