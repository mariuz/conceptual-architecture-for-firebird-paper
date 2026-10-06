# Sorting and Temporary Space

The [optimizer and execution document](query-optimizer-and-execution.md) shows `SORT (...)` appearing in plans and `SortedStream` sitting in record-source trees — but treats the sort itself as a black box. This document opens the box: the external merge sort in [`src/jrd/sort.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/sort.cpp) (key mangling, 128 KB buffers, runs, `RUN_GROUP` merges), the **TempSpace** layer that keeps sort data in memory up to `TempCacheLimit` and then spills to unlinked scratch files, and the **refetch** optimization (`InlineSortThreshold`) that keeps wide rows out of the sort entirely. Everything is demonstrated live — including catching a **448 MB `fb_sort_*` scratch file** in the server's file-descriptor table while its resident memory stayed pinned at the configured 64 MB — and compared with PostgreSQL's `work_mem`, MySQL's filesort and SQLite's sorter.

It completes the executor story of the [optimizer document](query-optimizer-and-execution.md) and the [aggregate/window document](aggregate-and-window-functions.md) (whose `SortedStream → AggregatedStream/WindowedStream` pipelines all begin here), and touches the [configuration story](deployment-and-operations.md) (three `firebird.conf` knobs) and [indexing](indexing-and-full-text-search.md) (index builds are sorts too).

**Table of Contents**

* [When a sort happens at all](#when-a-sort-happens-at-all)
* [Inside sort.cpp: diddled keys, runs and merges](#inside-sortcpp-diddled-keys-runs-and-merges)
* [TempSpace: memory first, then unlinked scratch files](#tempspace-memory-first-then-unlinked-scratch-files)
* [The refetch optimization: keeping wide rows out of the sort](#the-refetch-optimization-keeping-wide-rows-out-of-the-sort)
* [Sorting in action (validated)](#sorting-in-action-validated)
* [Comparison: PostgreSQL, MySQL, SQLite](#comparison-postgresql-mysql-sqlite)
* [Discussion](#discussion)
* [Further research](#further-research)

## When a sort happens at all

Firebird sorts in more places than `ORDER BY`. A `Sort` object is created for:

* **`ORDER BY`** that the optimizer can't satisfy by index navigation — `PLAN SORT (...)` instead of `PLAN ORDER (...)` ([the choice itself](query-optimizer-and-execution.md) is the optimizer's, weighing index walk vs sort cost);
* **`GROUP BY` and `DISTINCT`** — both are sort-then-scan in Firebird ([`SortedStream` feeding `AggregatedStream`](aggregate-and-window-functions.md)); `DISTINCT` uses the sort's built-in duplicate elimination callback;
* **window functions** — `WindowedStream` partitions and orders via sorts;
* **`MERGE JOIN`** inputs;
* **index creation** — `CREATE INDEX` sorts every key in the table before building the B-tree bottom-up (in FB5+ this sort is parallelized across `ParallelWorkers`).

Captured live: `SELECT FIRST 3 last_name FROM employee ORDER BY phone_ext` → `PLAN SORT ("PUBLIC"."EMPLOYEE" NATURAL)` — no index on `phone_ext`, so the executor's `SortedStream::internalOpen` pulls *every* row through the sort before the first row comes out. The sort is the classic **pipeline breaker**: `Sort::put()` all input, `Sort::sort()`, then `Sort::get()` one record at a time.

## Inside sort.cpp: diddled keys, runs and merges

`Sort` ([`sort.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/sort.cpp), \~2 400 lines descended straight from the InterBase tape-sort) stores fixed-length **sort records**: the key fields first, then whatever data must ride along, rounded up to longword alignment, capped at `MAX_SORT_RECORD` = 1 MB per record.

The first trick is **`diddleKey`** — called on every `put()` and undone on every `get()`. Each key field is *mangled in place* into a byte string whose plain `memcmp` order equals its SQL order: integers get their sign bit flipped, doubles get the IEEE bit-pattern transform, multi-byte integers are byte-swapped on little-endian machines, text is run through the [collation's](internationalization.md) string-to-key transform, NULLs are given a sortable prefix. After diddling, the entire comparison loop — quicksort and merge alike — is a single `memcmp` over the key bytes, with no per-type dispatch anywhere in the hot path.

The machinery around it is textbook external merge sort with Firebird-specific constants:

```mermaid
flowchart TD
    PUT["Sort::put — one sort record<br/>(key diddled on the way in)"] --> BUF["sort buffer<br/>max(record × MIN_RECORDS_TO_ALLOC, 128 KB)<br/>MAX_SORT_BUFFER_SIZE chunks"]
    BUF -->|"input ends and<br/>everything fit"| MEM["sortBuffer — quicksort<br/>(pre-pass straightens pairs,<br/>duplicate callback for DISTINCT)"]
    BUF -->|"buffer full"| RUN["putRun — quicksorted run<br/>written to TempSpace"]
    RUN --> TS["TempSpace<br/>memory blocks up to TempCacheLimit,<br/>then unlinked fb_sort_* scratch file"]
    TS -->|"RUN_GROUP = 8 runs<br/>merged at a time,<br/>MAX_MERGE_LEVEL = 2<br/>before re-merging"| MERGE["merge tree<br/>(sortRunsBySeek orders runs<br/>to minimize scratch-file seeks)"]
    MEM --> GET["Sort::get — records out in order<br/>(key un-diddled on the way out)"]
    MERGE --> GET
    GET --> SS["SortedStream::internalGetRecord<br/>→ AggregatedStream, WindowedStream,<br/>MergeJoin, ORDER BY output"]
```

_Figure 1: The external merge sort — diddled keys make every comparison a memcmp, full buffers become quicksorted runs in TempSpace, and runs merge eight at a time_

When input overflows the buffer, each buffer-full is quicksorted and flushed as a **run** (`putRun`); at `get()` time the runs are merged **eight at a time** (`RUN_GROUP = 8`), at most **two merge levels** deep (`MAX_MERGE_LEVEL = 2`) before intermediate re-merges are forced, and `sortRunsBySeek` orders run reads to keep scratch-file seeks sequential. None of this is novel — which is the point: the interesting engineering was pushed down a layer, into where the runs live.

## TempSpace: memory first, then unlinked scratch files

Runs are not written to files directly; they are written to a **`TempSpace`** ([`TempSpace.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/TempSpace.cpp)) — a virtual byte space, allocated in `TempBlockSize` chunks (default 1 MB), that is transparently **memory until a process-wide budget is exhausted, and disk after**. Three `firebird.conf` knobs govern it:

* **`TempCacheLimit`** — the total in-memory budget for all temp space in the process, shared across all attachments and sorts: default **64 MB** for SuperServer, **8 MB** for Classic/SuperClassic (same `ServerMode`-keyed defaulting as [`GCPolicy`](garbage-collection-and-sweep.md)), per-database overridable in `databases.conf`. Sorts small enough to fit under it never touch disk *even after overflowing their 128 KB sort buffers* — the "runs" live in RAM.
* **`TempBlockSize`** — the allocation granularity.
* **`TempDirectories`** — where scratch files land when the budget runs out: a semicolon-separated list tried in order, defaulting to `$FIREBIRD_TMP`, then the OS temp dir, then `/tmp`.

The scratch files are named `fb_sort_XXXXXX` (`SCRATCH` prefix in `sort.cpp`, created by [`TempFile`](https://github.com/FirebirdSQL/firebird/blob/master/src/common/classes/TempFile.cpp)) — and **unlinked immediately after creation** (`do_unlink = true`). The file exists only as an open descriptor: invisible to `ls`, impossible to leak across a crash, reclaimed by the kernel the moment the sort ends. The flip side, demonstrated below, is that observing one requires reading the server's `/proc/<pid>/fd` table — and that `df` can show a full temp partition with no visible file to blame.

## The refetch optimization: keeping wide rows out of the sort

Sorting `SELECT * ... ORDER BY one_column` naïvely drags every column through the sort records, the runs and the merges. Firebird's answer (the decision sits in [`Optimizer.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/optimizer/Optimizer.cpp), where `SortedStream`'s field map is built):

```mermaid
flowchart TD
    A["build sort record layout:<br/>keys + needed columns"] --> B{"totalLength ><br/>min(InlineSortThreshold = 1000,<br/>MAX_SORT_RECORD / 2)?"}
    B -->|no| INLINE["inline sort —<br/>whole rows ride<br/>through the sort"]
    B -->|yes| C{"any excludable fields?<br/>(persistent tables only —<br/>not views, not virtual,<br/>not external, not expressions)"}
    C -->|no| INLINE
    C -->|yes| REFETCH["refetch mode —<br/>sort only keys + DBKEYs,<br/>SortedStream::refetchRecord<br/>re-reads full rows afterwards"]
```

_Figure 2: The refetch decision — wide rows are sorted as key + record address, then re-read in sorted order_

If the projected sort record exceeds **`InlineSortThreshold`** (default 1 000 bytes), only the keys plus each contributing stream's **DBKEY** (physical record address) go through the sort; after sorting, `SortedStream::refetchRecord` fetches the full rows by address. The trade is explicit: much less data through buffer, runs and merges, in exchange for random re-reads in sorted order (and a subtle [read-committed caveat](transactions-and-concurrency.md): the refetched row is re-read *after* the sort, so it may be newer than the version sorted). The `FIRST ROWS` optimization mode forces refetch unconditionally — for a `FIRST 10 ORDER BY`, re-reading ten rows beats sorting wide records every time.

## Sorting in action (validated)

All against the live SuperServer (default config: `TempCacheLimit = 64M` confirmed in `firebird.conf`). The workload: a three-way cartesian join of `RDB$TYPES` (297 rows) generating 600 000 rows of ~820 bytes each — roughly **470 MB of sort data** — with `ORDER BY` on the padded string, so the key *is* the wide column and refetch can't help:

```
PLAN SORT (JOIN ("A" NATURAL, "B" NATURAL, "C" NATURAL))
```

Watching the engine process (not `fbguard` — the guardian is the parent; the engine is its `firebird` child) while the query ran, sampling `VmRSS` and the file-descriptor table every 150 ms:

```
RSS before 38856 kB, peak 112924 kB   (delta 72 MB)
peak fb_sort files: 1
peak scratch bytes: 469762048         (448 MB)
leftover files after query: none
```

The whole design in four numbers: resident memory grew by **72 MB** — the 64 MB `TempCacheLimit` plus working overhead — while **one** `fb_sort_*` descriptor (pointing at an already-deleted file) grew to **448 MB** of spilled runs, and vanished without cleanup the instant the query finished. The same query with `ROWS 25000` (~20 MB of sort data, under the budget): **zero scratch files** — runs stayed entirely in TempSpace's memory cache. Ordinary `ls /tmp` showed nothing in either case; only `/proc/<pid>/fd` reveals the spill.

## Comparison: PostgreSQL, MySQL, SQLite

| | **Firebird** | **PostgreSQL** | **MySQL** | **SQLite** |
|---|---|---|---|---|
| Memory knob | `TempCacheLimit` — **one process-wide budget** (64 MB SS default) | `work_mem` — **per sort node** (4 MB default), multiplied by every concurrent sort in every query | `sort_buffer_size` — per session (256 KB default) | heuristic; `SQLITE_CONFIG` / cache size |
| Spill | unlinked `fb_sort_*` in `TempDirectories` | `base/pgsql_tmp/*` files (visible, cleaned on restart) | files in `tmpdir` | temp files per `PRAGMA temp_store` / `SQLITE_TMPDIR` |
| Algorithm | quicksort runs + 8-way merge, memcmp on pre-mangled keys | quicksort / external merge; **top-N heapsort** under `LIMIT` | filesort: quicksort runs + merge | incremental merge sorter, multi-threaded with `PRAGMA threads` |
| Wide-row strategy | **refetch**: keys + DBKEY, re-read after (`InlineSortThreshold` 1000 B) | none — full tuples always travel through the sort | the same idea: rowid sort vs packed "addon fields" (old `max_length_for_sort_data`) | rowid+key sort natural to its B-tree design |
| Observability | plan `SORT` prefix; `/proc` for spill | `EXPLAIN ANALYZE`: "Sort Method: external merge Disk: … kB" — the gold standard | `filesort` in `EXPLAIN`, status counters | `EXPLAIN QUERY PLAN` "USE TEMP B-TREE" |

Three contrasts carry the story:

* **Budget shape.** PostgreSQL's `work_mem` is per-operator: a query with four sorts may use 4×, a hundred connections may use 400× — under-provisioning is the default posture and DBAs tune it endlessly. Firebird's `TempCacheLimit` is one global pot: no multiplication surprise, but a single huge sort can eat the whole budget and push every other concurrent sort to disk. Neither shape dominates; they fail differently.
* **Wide rows.** MySQL's rowid-sort-vs-addon-fields choice is exactly Firebird's refetch-vs-inline choice under another name (both engines even default the threshold near 1 KB) — convergent evolution on a real trade-off. PostgreSQL, notably, never refetches: its sorts always carry full tuples, one reason its `work_mem` pressure is felt so keenly.
* **Top-N.** PostgreSQL's heapsort under `LIMIT` keeps only N tuples in memory — Firebird has no equivalent (the `Sort` class carries a `max_records` field annotated *"assigned but unused"*); a `FIRST 1 ... ORDER BY` over 600 k rows sorts all 600 k, as the demo's scratch file proves. The refetch fast-path under `FIRST ROWS` mitigates the width, not the count. This is Firebird's clearest missing sort optimization.

## Discussion

The sort subsystem shows its lineage more openly than any other corner of the engine — `RUN_GROUP`, tape-merge vocabulary, a comment citing the VAX — and yet its architecture is exactly where a modern engine would land: comparisons reduced to `memcmp` by pre-transforming keys (the same idea as PostgreSQL's abbreviated keys or an LSM's key encoding), spill managed by a dedicated caching layer rather than the sort itself, and row width attacked *before* the sort rather than endured inside it. The one visible gap is top-N. For the operator's day-to-day: `PLAN SORT` vs `PLAN ORDER` tells you a sort exists; `TempCacheLimit` decides whether it stays silent in RAM; and when a temp partition mysteriously fills with nothing in `ls`, this document's `/proc` trick is the diagnosis.

## Hands-on: samples, tests and debugging

### C++ sample — [`samples/cpp/sorting.cpp`](samples/cpp/sorting.cpp)

The [TempCacheLimit threshold](#tempspace-memory-first-then-unlinked-scratch-files) reproduced at a size kind to a small machine: 200,000 rows with a 400-byte ASCII key (~82 MB of sort data — the key *is* the wide column, so [refetch](#the-refetch-optimization-keeping-wide-rows-out-of-the-sort) can't shrink it), against a 20,000-row (~8 MB) variant of the same `ORDER BY`. While each query runs, a watcher thread samples both sides of the story: the server's `/proc/<pid>/fd` table (via `sudo`) for the **unlinked** `fb_sort_*` scratch files that `ls` can never show, and database-level `MON$MEMORY_USAGE` from a second attachment — one fresh transaction per poll, because MON$ snapshots are per-transaction. The server pid comes from `MON$ATTACHMENTS.MON$SERVER_PID`; run it on the server machine with passwordless sudo.

```sh
cmake -B build samples && cmake --build build
./build/sorting        # default: inet://localhost//tmp/fbhandson/sorting.fdb
```

Verified output:

```text
bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data
server pid 215035, database memory allocated while idle: 28151808 bytes

big sort (200k rows, ~82 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  top row id = 195722
  peak fb_sort_* scratch: 1 file(s), 73400320 bytes
  peak database MON$MEMORY_ALLOCATED: 96116736 bytes (+67964928 over idle)

small sort (20k rows, ~8 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  top row id = 65950
  peak fb_sort_* scratch: 0 file(s), 0 bytes
  peak database MON$MEMORY_ALLOCATED: 47923200 bytes (+19771392 over idle)

done.
```

The same four-number story as the [448 MB demonstration](#sorting-in-action-validated), at one-sixth scale: the big sort's allocated memory grows by **+68 MB** — the 64 MB `TempCacheLimit` plus block overhead — and *one* scratch descriptor absorbs the remaining **70 MB** of runs; the small sort stays under budget, and the scratch count is **zero** even though it, too, overflowed its 128 KB sort buffers — runs live in TempSpace's memory cache. Both queries print the same `PLAN SORT (...)`, which is the point: the plan tells you a sort exists, only the volume decides where it lives.

### fb-cpp sample — [`samples/fb-cpp/sorting.cpp`](samples/fb-cpp/sorting.cpp)

The same two-eyed experiment through [fb-cpp](https://github.com/asfernandes/fb-cpp) (vendored at [`extern/fb-cpp`](extern/fb-cpp)), the modern C++20 wrapper over the OO API. The `/proc/<pid>/fd` half is untouched — no wrapper can abstract the server's fd table, so the scratch-file watcher is the same `popen` over `find`/`stat` — but the MON$ half is where the wrapper shows: each poll opens a fresh RAII `Transaction` (new transaction, new MON$ snapshot), `MON$MEMORY_ALLOCATED` comes back as `std::optional<int64_t>` from `getInt64` instead of a string fed to `atol`, and the plans arrive prefetched via `StatementOptions().setPrefetchLegacyPlan(true)` rather than a separate info call.

```sh
cmake -B build samples && cmake --build build   # needs libboost-dev + libboost-filesystem-dev
./build/fbcpp_sorting
```

Verified: the same threshold signature — the big sort peaks at 1 unlinked `fb_sort_*` file of exactly 73400320 bytes (70 MB of runs) with MON$ growth of +68358144 over a 26255360-byte idle; the small sort shows 0 scratch files and +20099072 of memory growth; both print the identical `PLAN SORT ("PUBLIC"."BULK" NATURAL)`.

### JavaScript sample — [`samples/nodejs/sorting.js`](samples/nodejs/sorting.js)

The MON$ half of the same experiment through the wire protocol (`cd samples/nodejs && node sorting.js`; it reuses the C++ sample's `bulk` table, building it if missing). One connection sorts, a second polls `MON$MEMORY_USAGE` — node-firebird runs each `db.query` in its own transaction, so every poll is a fresh MON$ snapshot with no extra code. Verified peaks: `+67506176 over idle` for the big sort, `+19378176` for the small one — the same threshold signature, without the `/proc` half (a browser-less Node process could run remotely; the fd table only exists on the server box).

### Rust sample — [`samples/rust/src/bin/sorting.rs`](samples/rust/src/bin/sorting.rs)

Both halves of the experiment through [rsfbclient](https://github.com/fernandobatels/rsfbclient), Rust's Firebird client (`cd samples/rust && cargo run --bin sorting`). The watcher runs on its *own* connection — Rust's ownership rules won't let a second thread borrow the main attachment, so the two-attachment discipline the C++ sample chose by design is enforced here by the compiler — and each MON$ poll is a fresh `SimpleTransaction` whose `query_first` hands back `MON$MEMORY_ALLOCATED` as a typed `(i64,)` tuple. The `/proc/<pid>/fd` half is the same shell-out as everywhere else. The one real delta: rsfbclient has no `getPlan`, so instead of the legacy plan string the sample feeds each query to Firebird 6's `RDB$SQL.EXPLAIN` — and gets back *more* than `PLAN SORT (...)`: the explained tree prints the Sort node with its record and key lengths, the very numbers the 82 MB estimate is made of.

Verified: the explained plan shows `Sort (record length: 430, key length: 408)` under a `Refetch` node — 408 of the 430 bytes *are* the key, confirming why refetch cannot shrink this particular sort. The threshold signature is identical to both C++ twins: the big sort peaks at 1 unlinked `fb_sort_*` file of exactly 73400320 bytes with MON$ growth of +68620288 over a 21008384-byte idle; the small sort shows 0 scratch files, 0 bytes, and +20492288 of in-memory growth.

### Free Pascal sample — [`samples/fpc/sorting.pas`](samples/fpc/sorting.pas)

Both halves of the experiment once more, through [fbintf](https://github.com/MWASoftware/fbintf) (vendored at [`extern/fbintf`](extern/fbintf)), MWA Software's Firebird Pascal API — the layer under IBX, driving the same libfbclient behind COM-style reference-counted interfaces (`make -C samples/fpc bin/sorting && samples/fpc/bin/sorting`). The watcher is a classic `TThread` whose `Execute` opens its own `IAttachment`, and each MON$ poll collapses to a one-liner — `Mon.OpenCursorAtStart(Tr, MON_SQL)[0].AsInt64` inside a fresh `StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency], taCommit)` — while the `/proc/<pid>/fd` half is the same shell-out as everywhere, here through FPC's `POpen`. The plan story lands between the neighbors: fbintf *has* `Stmt.GetPlan` where rsfbclient has nothing, but it returns only the detailed plan, never the legacy one-liner — which for this document is the right quirk, since the explained tree carries the Sort node's record and key lengths without any `RDB$SQL.EXPLAIN` detour.

Verified: both plans print `Sort (record length: 430, key length: 408)` under a `Refetch` node; the big sort peaks at 1 unlinked `fb_sort_*` file of exactly 73400320 bytes with MON$ growth of +67964928 over a 28217344-byte idle; the small sort shows 0 scratch files, 0 bytes, and +19836928 of in-memory growth — the same threshold signature as all four twins above.

### Python sample — [`samples/python/sorting.py`](samples/python/sorting.py)

The same experiment through [firebird-driver](https://github.com/FirebirdSQL/python3-driver), the FirebirdSQL project's own Python driver over libfbclient's OO API (`python3 samples/python/sorting.py`). The watcher is a plain `threading.Thread` on its own attachment — the driver releases the GIL inside its ctypes calls, so it keeps polling while the main thread is blocked in the fetch that performs the sort — and each MON$ poll is a fresh read-only `TransactionManager` built from a typed `TPB`. The plan surface is the most complete of the wrapper family: the prepared `Statement` offers both `.plan` (the legacy `PLAN SORT (...)`) and `.detailed_plan`, whose Sort node carries the record and key lengths. The scratch half is the honest gap — not of the driver but of privilege: the unlinked `fb_sort_*` files are visible only in the server's `/proc/<pid>/fd`, which needs the server's uid or root, and this run had neither (`FB_SORT_SUDO=1` makes the sample use `sudo -n` like the C++ twin). So it falls back to a root-free witness: an unlinked file still occupies its blocks, and the free space of the scratch filesystem (`/tmp`, the default `TempDirectories`) drops by the spill while the big sort runs.

Verified output:

```text
bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data
server pid 665, database memory allocated while idle: 26025984 bytes

big sort (200k rows, ~82 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  -> Sort (record length: 430, key length: 408)
  top row id = 41552
  /proc/665/fd not readable; peak drop of free space on /tmp: 73408512 bytes
  peak database MON$MEMORY_ALLOCATED: 94384128 bytes (+68358144 over idle)

small sort (20k rows, ~8 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  -> Sort (record length: 430, key length: 408)
  top row id = 15690
  /proc/665/fd not readable; peak drop of free space on /tmp: 0 bytes
  peak database MON$MEMORY_ALLOCATED: 34639872 bytes (+8613888 over idle)

done.
```

The free-space drop of 73408512 bytes is the twins' 73400320-byte scratch file plus two 4 KB filesystem blocks of noise — the spill measured from outside the process, without reading its fd table — and the big sort's MON$ growth (+68 MB) matches every other twin. (The fallback measures the whole filesystem, so another process writing to `/tmp` at the same moment would show up in it.)

### Go sample — [`samples/go/sorting/main.go`](samples/go/sorting/main.go)

The same experiment through [firebirdsql](https://github.com/nakagami/firebirdsql), a pure-Go implementation of the wire protocol behind `database/sql` (`cd samples/go && go run ./sorting`). The watcher is a goroutine with its own `*sql.DB` — its own attachment — and needs none of the Python twin's GIL reasoning: the pure-Go driver blocks only the goroutine waiting on its socket, so polling continues while the main goroutine sits in the fetch that performs the sort; each MON$ poll is a fresh `BeginTx`, hence a fresh snapshot. firebirdsql has no plan API, so the Sort node comes from Firebird 6's `RDB$SQL.EXPLAIN` — where, as the sample's comment notes, it shares a row with the `Refetch` above it — and it carries the same record and key lengths as `.detailed_plan`. The scratch half has the same privilege gap as the Python twin and takes the same root-free fallback (`FB_SORT_SUDO=1` switches to `sudo -n`): the free space of `/tmp` drops by exactly the 73400320-byte spill while the big sort runs, and not at all for the small one.

Verified output:

```text
bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data
server pid 665, database memory allocated while idle: 26091520 bytes

big sort (200k rows, ~82 MB)
  -> Sort (record length: 430, key length: 408)
  top row id = 156868
  /proc/665/fd not readable; peak drop of free space on /tmp: 73400320 bytes
  peak database MON$MEMORY_ALLOCATED: 96419840 bytes (+70328320 over idle)

small sort (20k rows, ~8 MB)
  -> Sort (record length: 430, key length: 408)
  top row id = 91300
  /proc/665/fd not readable; peak drop of free space on /tmp: 0 bytes
  peak database MON$MEMORY_ALLOCATED: 48160768 bytes (+22069248 over idle)

done.
```

Both MON$ figures are measured against the one idle baseline taken before the first sort, so the small sort's +22 MB also carries whatever the big sort's pools had not yet given back; what matters is the scratch column — a 70 MB spill above `TempCacheLimit`, nothing below it.

### Java sample — [`samples/java/src/main/java/fbsamples/Sorting.java`](samples/java/src/main/java/fbsamples/Sorting.java)

The same experiment through [Jaybird](https://github.com/FirebirdSQL/jaybird), the FirebirdSQL project's JDBC driver, on its default pure-Java wire protocol (`cd samples/java && mvn -q compile exec:exec -Dsample=Sorting`). The watcher is a plain `Thread` with its own `Connection`, which means its own attachment. Like the pure-Go driver, Jaybird blocks only the thread that waits on its socket, so polling continues while the main thread sits in the `executeQuery` that performs the sort. Each poll ends in `commit()`, so the next poll gets a new MON$ snapshot. The plan surface matches firebird-driver's: `FirebirdPreparedStatement.getExecutionPlan()` gives the legacy `PLAN SORT (...)`, and `getExplainedExecutionPlan()` gives the tree whose Sort node carries the record and key lengths. The scratch half has the same privilege gap as the Python and Go twins. It tries the server's `/proc/<pid>/fd` first and otherwise watches the free space of `/tmp` through `java.nio.file.FileStore.getUsableSpace()`. The peak drop is the familiar 73400320-byte spill plus 84 KB of filesystem noise.

Verified output:

```text
bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data
server pid 665, database memory allocated while idle: 26288128 bytes

big sort (200k rows, ~82 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  -> Sort (record length: 430, key length: 408)
  top row id = 39140
  /proc/665/fd not readable; peak drop of free space on /tmp: 73486336 bytes
  peak database MON$MEMORY_ALLOCATED: 94384128 bytes (+68096000 over idle)

small sort (20k rows, ~8 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  -> Sort (record length: 430, key length: 408)
  top row id = 39140
  /proc/665/fd not readable; peak drop of free space on /tmp: 0 bytes
  peak database MON$MEMORY_ALLOCATED: 46125056 bytes (+19836928 over idle)

done.
```

### C# sample — [`samples/csharp/Samples/Sorting.cs`](samples/csharp/Samples/Sorting.cs)

The same experiment through [FirebirdClient](https://github.com/FirebirdSQL/NETProvider) (`FirebirdSql.Data.FirebirdClient`), the FirebirdSQL project's ADO.NET provider, on its default managed wire protocol (`cd samples/csharp && dotnet run -- Sorting`). The watcher is a dedicated `Thread` with its own `FbConnection`, which means its own attachment, and every poll runs in a fresh `FbTransaction`, so it gets a new MON$ snapshot. As with Jaybird and the pure-Go driver, the managed protocol blocks only the thread waiting on its socket, so polling continues while the main thread sits in the first `Read()`, which performs the sort. Both plan forms come from the prepared `FbCommand`: `GetCommandPlan()` gives `PLAN SORT (...)` and `GetCommandExplainedPlan()` gives the tree whose Sort node carries the record and key lengths. The scratch half has the familiar privilege gap. The sample tries the server's `/proc/<pid>/fd` first (a link's `LinkTarget`, then `File.OpenHandle` + `RandomAccess.GetLength` on the unlinked file) and otherwise watches `/tmp`'s free space through `DriveInfo.AvailableFreeSpace`. The big sort's peak drop is the 73400320-byte spill plus some 5 MB of noise from other work on the shared `/tmp` during this run. The small sort stays in memory.

Verified output:

```text
bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data
server pid 665, database memory allocated while idle: 24776704 bytes

big sort (200k rows, ~82 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  -> Sort (record length: 430, key length: 408)
  top row id = 55132
  /proc/665/fd not readable; peak drop of free space on /tmp: 78778368 bytes
  peak database MON$MEMORY_ALLOCATED: 92545024 bytes (+67768320 over idle)

small sort (20k rows, ~8 MB)
  PLAN SORT ("PUBLIC"."BULK" NATURAL)
  -> Sort (record length: 430, key length: 408)
  top row id = 24890
  /proc/665/fd not readable; peak drop of free space on /tmp: 8192 bytes
  peak database MON$MEMORY_ALLOCATED: 44417024 bytes (+19640320 over idle)

done.
```

### Things to try

- Drop the `desc` and `first 1` and fetch everything: the numbers barely move — the sort is a pipeline breaker, so the *open* pays for the whole sort whether you fetch one row or all 200,000.
- Change the query to `select first 1 pad from bulk order by id` after `create index bulk_id on bulk (id)`: the plan flips to `ORDER` (index navigation), and both the memory delta and the scratch file vanish — no `Sort` object is ever created.
- Sort `select id, pad from bulk order by id` (key = 4-byte int, payload = 400-byte pad, total > `InlineSortThreshold`): watch whether refetch mode kicks in — the scratch volume should collapse to keys + DBKEYs.
- Halve the workload (`where mod(id, 2) = 0`, ~41 MB): still under the 64 MB budget plus overhead? Find the row count where the first `fb_sort_*` file appears — that *is* `TempCacheLimit`, measured from outside.

### Debugging this in C++ (gdb)

With a [debug build of the engine](debugging-firebird.md), the whole pipeline of Figure 1 is breakpointable:

```gdb
break SortedStream::internalOpen  # src/jrd/recsrc/SortedStream.cpp:60 — the pipeline break: put-all, sort, then get
break Sort::put                   # src/jrd/sort.cpp:333  — one sort record in (key just diddled)
break Sort::putRun                # src/jrd/sort.cpp:2008 — a full buffer quicksorted and flushed as a run
break TempSpace::setupFile        # src/jrd/TempSpace.cpp:504 — TempCacheLimit exhausted: the spill moment
break TempFile::create            # src/common/classes/TempFile.cpp:126 — fb_sort_* created (and unlinked)
break Sort::get                   # src/jrd/sort.cpp:299  — records out in order (key un-diddled)
```

Run the sample's big sort and the breakpoints fire in exactly that order; the small sort never reaches `TempSpace::setupFile` — the threshold behavior as a breakpoint that does or doesn't hit. At `Sort::put`, the record at `*record_address` shows the diddled key bytes (compare a negative and positive integer key to see the sign-bit flip); at `Sort::putRun` the run being flushed goes to `m_space` — a `TempSpace` whose `head` chain is memory blocks until `setupFile`'s backtrace shows the budget check failing; and `TempFile`'s `doUnlink` handling (`TempFile.cpp:250`) is the two-line reason `ls /tmp` shows nothing while `/proc/<pid>/fd` shows hundreds of megabytes. See the [debugging guide](debugging-firebird.md).

## Further research

* [`src/jrd/sort.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/sort.cpp) / [`sort.h`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/sort.h) — `put`/`sort`/`get`, `diddleKey`, `putRun`, the merge tree; the header comments are a history lesson.
* [`src/jrd/TempSpace.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/TempSpace.cpp) / [`src/common/classes/TempFile.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/common/classes/TempFile.cpp) — the memory/disk split and the unlink-on-create discipline.
* [`src/jrd/recsrc/SortedStream.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/recsrc/SortedStream.cpp) — the executor face: `internalOpen` (the pipeline break), `mapData`, `refetchRecord`.
* [`src/jrd/optimizer/Optimizer.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/optimizer/Optimizer.cpp) — the refetch decision and sort-vs-index-order costing.
* PostgreSQL docs: [Resource Consumption — work_mem](https://www.postgresql.org/docs/current/runtime-config-resource.html) · MySQL docs: [ORDER BY Optimization](https://dev.mysql.com/doc/refman/8.4/en/order-by-optimization.html) (the filesort variants).
* Companion docs: [optimizer and execution](query-optimizer-and-execution.md) (who decides to sort) · [aggregates and windows](aggregate-and-window-functions.md) (who consumes sorts) · [deployment](deployment-and-operations.md) (where the knobs live) · [internationalization](internationalization.md) (collation keys feeding `diddleKey`).
