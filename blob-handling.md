# BLOB and Large-Object Handling

Rows are for small, fixed-ish values; documents, images, JSON blobs and multi-megabyte text need a different storage path. This document describes how Firebird 6 stores and manipulates **BLOBs** (Binary Large OBjects) — the separate-storage model, the multi-level page addressing, subtypes and charsets, segmented and stream access, and the FB4/FB5 manipulation functions — grounded in the vendored source (`ods.h`, `blb.cpp`, the blob SQL-extension docs) and demonstrated live, then compares the large-object story with PostgreSQL, MySQL and SQLite.

It builds on the [on-disk structure document](on-disk-structure.md) (blob pages in the file), the [SQL dialect and data types document](sql-dialect-and-types.md) (BLOB as a type), the [internationalization document](internationalization.md) (text-blob charsets), and the [wire-protocol document](firebird-wire-protocol.md) (blob transfer on the wire).

**Table of Contents**

* [BLOBs live outside the record](#blobs-live-outside-the-record)
* [Multi-level page addressing](#multi-level-page-addressing)
* [Subtypes, charsets and filters](#subtypes-charsets-and-filters)
* [Segmented and stream access](#segmented-and-stream-access)
* [Manipulating BLOBs: BLOB_APPEND and RDB$BLOB_UTIL](#manipulating-blobs-blob_append-and-rdbblob_util)
* [BLOB internals in action (validated)](#blob-internals-in-action-validated)
* [Comparison: PostgreSQL, MySQL, SQLite](#comparison-postgresql-mysql-sqlite)
* [Discussion](#discussion)
* [Further research](#further-research)

## BLOBs live outside the record

The defining Firebird decision: a BLOB's *data* is stored **separately from the record**. The record (on its [data page](on-disk-structure.md#inside-a-data-page-records-and-version-deltas)) holds only a small **blob id** — a pointer — while the bytes live on their own **blob pages** (`pag_blob`). A **blob header** (`struct blh` in `ods.h`) describes each BLOB: the lead page, page count (`blh_max_sequence`), segment count (`blh_count`), total length (`blh_length`, a 64-bit value), longest segment, subtype, charset, and the addressing **level**.

```mermaid
flowchart LR
    subgraph DP["data page"]
        REC["record<br/>(regular columns +<br/>blob id pointer)"]
    end
    REC -->|"blob id"| BLH["blob header (blh)<br/>length, segments, subtype,<br/>charset, level"]
    BLH --> BLOB[("blob pages<br/>(pag_blob)<br/>the actual bytes")]
```

_Figure 1: A record holds only a blob id; the BLOB's bytes live on separate blob pages, described by a blob header_

This is why, in the live demo below, a table with multi-kilobyte text blobs still reports an **average record length of ~15 bytes** — the blob bytes are not in the record. It keeps records small and uniform (good for the [MVCC page layout](transactions-and-concurrency.md)), lets a query that doesn't touch the blob column avoid reading blob pages entirely, and means updating a row's scalar columns doesn't rewrite the blob.

## Multi-level page addressing

Firebird addresses a BLOB's pages with up to three levels (`blb.cpp`, `blh_level`), so one mechanism scales from a few bytes to gigabytes:

```mermaid
flowchart TB
    L0["Level 0 — tiny blob<br/>data held with the record / one place"]
    L1["Level 1 — the blob header points<br/>directly at a list of data pages"]
    L2["Level 2 — the header points at a<br/>blob POINTER page (blp_pointers),<br/>which points at the data pages"]
    L0 -->|"grows"| L1 -->|"grows"| L2
    L2 --> DPs[("many data pages")]
```

_Figure 2: BLOB address levels — small BLOBs are stored directly, larger ones through a page list, the largest through a pointer page (a level of indirection), so the maximum size scales with page size_

- **Level 0** — a very small BLOB kept in place.
- **Level 1** — the header references data pages directly; suits BLOBs up to roughly a page-list's worth.
- **Level 2** — the header references a **blob pointer page** (`blp_pointers` flag) whose entries reference the data pages, adding one level of indirection so very large BLOBs fit. With a 32 KB [page size](on-disk-structure.md#setup-and-administration) the ceiling reaches into the gigabytes.

The engine promotes a BLOB to a higher level automatically as it grows; the application never chooses a level.

## Subtypes, charsets and filters

Unlike a plain "bag of bytes", a Firebird BLOB is **typed** by a subtype (`blh_sub_type`, from `consts_pub.h`):

| Subtype | Name | Meaning |
|---|---|---|
| 0 | `BINARY` (untyped) | Arbitrary bytes |
| 1 | `TEXT` | Text, with an associated character set |
| 2 | `BLR` | Compiled procedure/trigger code (internal) |
| 3–6 | ACL, ranges, summary, format | Internal engine uses |
| negative | user-defined | Application-defined subtypes |

**Text BLOBs carry a character set** (`blh_charset`) just like a `VARCHAR` — so `SUBSTRING`, `UPPER`, and comparison work correctly on large text, tying into the [internationalization subsystem](internationalization.md). A **BLOB filter** is a pluggable converter between subtypes (registered with `DECLARE FILTER`); reading a BLOB "as" another subtype runs it through the filter — a small, unusual extensibility point for on-the-fly transformation.

## Segmented and stream access

BLOBs are not read or written as one monolithic value but in **segments** — chunks delivered one at a time, so a gigabyte BLOB never needs to be fully in memory. The [OO API](client-apis-and-drivers.md)'s `IBlob` exposes `getSegment`/`putSegment`, and the [wire protocol](firebird-wire-protocol.md#packet-model-opcodes-and-xdr) has dedicated opcodes (`op_get_segment`, `op_put_segment`, `op_open_blob`, `op_create_blob`). A BLOB is either **segmented** (the classic mode, remembering segment boundaries — `blh_count`, longest segment) or a **stream BLOB** (`rhd_stream_blob`, a flat byte stream with no segment structure, better for random access via seek). Firebird 5 also added **inline BLOBs** on the wire (`op_inline_blob`, protocol 19 — see the [wire-protocol version table](firebird-wire-protocol.md#protocol-versions)): small BLOBs are shipped *with* the result row instead of requiring a separate open/fetch round-trip, a meaningful latency win for rows with small blobs.

## Manipulating BLOBs: BLOB_APPEND and RDB$BLOB_UTIL

Historically, building a BLOB in SQL/PSQL meant concatenation that recopied the whole value each time. Firebird added efficient primitives:

- **`BLOB_APPEND`** (FB4, [`README.blob_append.md`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.blob_append.md)) — appends fragments to a BLOB in **temporary storage** without recopying, e.g. `BLOB_APPEND(base, 'a', 'b', 'c')`. Ideal for assembling large text in a loop.
- **`RDB$BLOB_UTIL`** package (FB5, [`README.blob_util.md`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.blob_util.md)) — lower-level BLOB manipulation that standard functions can't do or do slowly: `NEW_BLOB` (create with explicit `SEGMENTED`/`TEMP_STORAGE` options), plus reading, seeking and info functions operating on binary data directly, even for text BLOBs.

Temporary BLOBs (what `BLOB_APPEND` creates) have transient ids until materialized; the [`SET TRANSACTION`](transactions-and-concurrency.md#savepoints-explicit-locks-and-autonomous-transactions) `AUTO RELEASE TEMP BLOBID` option manages their memory for mass inserts.

## BLOB internals in action (validated)

Real output from a live Firebird 6 server (a `docs` table with a text BLOB and a binary BLOB):

```sql
CREATE TABLE docs (
  id INTEGER PRIMARY KEY,
  note BLOB SUB_TYPE TEXT CHARACTER SET UTF8,   -- subtype 1, charset-aware
  data BLOB SUB_TYPE BINARY                       -- subtype 0
);
INSERT INTO docs (id, note) VALUES (3, BLOB_APPEND(CAST('' AS BLOB SUB_TYPE TEXT), 'part1-', 'part2-', 'part3'));
```

The blob columns' subtype and charset, from the catalog:

```text
FIELD    SUBTYPE    CHARSET
note        1       UTF8       -- text blob: subtype 1, carries a charset
data        0       <null>     -- binary blob: subtype 0, no charset
```

Blob lengths and the `BLOB_APPEND` result:

```text
ID   NOTE_BYTES   NOTE_CHARS
1        10           10        -- 'short note'
2      3903         3903        -- larger text (multi-page blob)
3        17           17        -- BLOB_APPEND('part1-','part2-','part3') = 'part1-part2-part3'
```

And the proof that BLOB data lives outside the record — `gstat` on the same table:

```text
"PUBLIC"."DOCS" (128)
    Average record length: 14.67, total records: 3
```

The average record is **~15 bytes** even though row 2 holds ~3.9 KB of text — because the record stores only the blob id, and the 3.9 KB lives on separate blob pages. This is the separate-storage model made visible.

## Comparison: PostgreSQL, MySQL, SQLite

| Aspect | **Firebird** | **PostgreSQL** | **MySQL** | **SQLite** |
|---|---|---|---|---|
| Model | **Out-of-line** blob pages + blob id | [TOAST](https://www.postgresql.org/docs/current/storage-toast.html) (auto off-page) *or* [large objects](https://www.postgresql.org/docs/current/largeobjects.html) | Inline + [off-page overflow](https://dev.mysql.com/doc/refman/8.4/en/innodb-row-format.html) (row format) | Inline + [overflow pages](https://sqlite.org/intern-v-extern-blob.html) |
| Types | `BLOB SUB_TYPE TEXT/BINARY` (+ user subtypes) | `bytea` / `text` (TOAST); LO `oid` | `TINY..LONGBLOB`/`TEXT` | `BLOB` storage class |
| Typed / charset | **Subtypes + text charset** | text encoding (DB-wide) | charset on TEXT | none |
| Streaming API | **Segments** (`IBlob`, wire opcodes) | LO [`lo_read`/`lo_write`](https://www.postgresql.org/docs/current/largeobjects.html); `bytea` whole | whole value (no core streaming) | [Incremental BLOB I/O](https://sqlite.org/c3ref/blob_open.html) |
| Build/append efficiently | `BLOB_APPEND`, `RDB$BLOB_UTIL` | `||` (rewrites) / LO writes | `CONCAT` (rewrites) | `||` / incremental write |
| Max size | Page-size dependent (GB range) | 1 GB (`bytea`/TOAST); 4 TB (LO) | 4 GB (`LONGBLOB`) | ~2 GB ([limits](https://sqlite.org/limits.html)) |
| Read without loading column | Yes (record excludes blob) | Yes (TOAST/LO on demand) | Depends on row format | Yes (incremental I/O) |
| Filters / transform | **BLOB filters** (subtype converters) | via functions/extensions | via functions | via app functions |

## Discussion

**Firebird and PostgreSQL large objects share the out-of-line philosophy; TOAST, MySQL and SQLite lean inline-with-overflow.** Firebird always stores BLOB bytes on separate pages referenced by a blob id — the record never carries the payload, so scalar-only queries and updates never touch blob storage, and records stay small and uniform. PostgreSQL's *large objects* work similarly (a separate `pg_largeobject` store with an oid handle and a streaming API), but its *default* path for `bytea`/`text` is TOAST: the value lives inline until it's big, then is compressed and pushed to a side table transparently. MySQL and SQLite are inline-first too, spilling to overflow pages when a value won't fit. Firebird's always-separate model is the most consistent — no size threshold, no row-format subtlety — at the cost that even small BLOBs pay a little indirection (which FB5's inline-blob wire optimization specifically claws back).

**Firebird's typed, charset-aware BLOBs are distinctive.** Alone among the four, Firebird gives BLOBs a **subtype** (text vs binary vs user-defined) and text BLOBs a **character set**, so large text participates in collation and string functions the same way a `VARCHAR` does, and applications can tag BLOBs with their own subtypes and register **filters** to convert between them. The others treat a large object as essentially untyped bytes (with a text/binary distinction and, for text, a database-wide encoding). This typing is a small but real expressiveness advantage for document-oriented data.

**Streaming is the shared necessity, solved four ways.** Nobody wants a gigabyte value fully in memory, so each engine offers incremental access: Firebird's segment API and wire opcodes, PostgreSQL's large-object `lo_read`/`lo_write`, SQLite's incremental BLOB I/O (`sqlite3_blob_open`). MySQL is the laggard here — its core protocol transfers whole values, so streaming is an application concern. This mirrors a theme from the [client-APIs document](client-apis-and-drivers.md): Firebird's protocol and API were designed around chunked large-object transfer from the start, part of the same design that produced its separate-storage model.

## Hands-on: samples, tests and debugging

### C++ sample — [`samples/cpp/blobs.cpp`](samples/cpp/blobs.cpp)

The [segmented access model](#segmented-and-stream-access) exercised directly through `IBlob`: the sample creates a text blob with three explicit `putSegment()` calls, stores it via a parameterized `INSERT` (the message buffer receives only the 8-byte `ISC_QUAD` blob id — the record-holds-a-pointer model of the [first section](#blobs-live-outside-the-record)), reads it back with a `getSegment()` loop that returns the *same three segments with their boundaries intact*, and then asks the blob to describe itself with `getInfo()` (segment count, longest segment, total length, segmented-vs-stream type — the client-visible face of `struct blh`). It closes with the catalog view of [subtype and charset](#subtypes-charsets-and-filters) for a `TEXT` vs `BINARY` column and a [`BLOB_APPEND`](#manipulating-blobs-blob_append-and-rdbblob_util) round trip.

```sh
cmake -B build samples && cmake --build build
./build/blobs        # default: inet://localhost//tmp/fbhandson/blobs.fdb
```

Verified output:

```text
wrote 3 segments into a new text blob
  getSegment #1: 13 bytes  "first segment"
  getSegment #2: 22 bytes  "second, longer segment"
  getSegment #3:  5 bytes  "third"
blob info: 3 segments, longest 22, total 40 bytes, type 0 (0=segmented)

-- column subtypes (RDB$FIELDS) --
FIELD SUBTYPE CHARSET
----- ------- -------
DATA  0       <null>
NOTE  1       UTF8

-- BLOB_APPEND result --
ID OCTETS CHARS CONTENT
-- ------ ----- -----------------
2  17     17    part1-part2-part3
done.
```

The blob-info line is the segmented model in one line: the engine remembered not just 40 bytes but *three segments, the longest 22* — exactly the `blh_count`/`blh_max_segment` bookkeeping this document describes on disk.

### fb-cpp sample — [`samples/fb-cpp/blobs.cpp`](samples/fb-cpp/blobs.cpp)

The same scenario through [fb-cpp](https://github.com/asfernandes/fb-cpp) (vendored at [`extern/fb-cpp`](extern/fb-cpp)), the modern C++20 wrapper over the OO API — and here the wrapper's own abstraction is the subject: `fbcpp::Blob` is a RAII object with `writeSegment`/`readSegment` over `std::span` (the segmented model, boundaries kept), the blob id travels through `Statement::setBlobId`/`getBlobId` as a typed value instead of an `ISC_QUAD` in a message buffer, and the raw BPB becomes a `BlobOptions` builder — `setType(BlobType::STREAM)` instead of hand-packed `isc_bpb_type` bytes, which lets the sample add a stream twin of the same 40 bytes and call `seek()` where a segmented blob would refuse it. One thing fb-cpp does *not* surface is the `blh_count`/`blh_max_segment` bookkeeping, so the sample drops to `getHandle()->getInfo()` — the escape hatch to the underlying OO-API object that every wrapper class provides.

```sh
cmake -B build samples && cmake --build build   # needs libboost-dev + libboost-filesystem-dev
./build/fbcpp_blobs
```

Verified: `readSegment` returns the same three segments with boundaries intact (13, 22, 5 bytes), `Blob::getLength() = 40 bytes`, and the raw `getInfo` via `getHandle()` reports `3 segments, longest 22, type 0 (0=segmented)`; the catalog and `BLOB_APPEND` checks match the OO-API run (`NOTE 1 UTF8`, `17 17 part1-part2-part3`). The stream twin shows the contrast: one `readSegment` call returns all 40 bytes in one chunk — `"first segmentsecond, longer segmentthird"`, no boundaries — and after `seek(FROM_BEGIN, 35)` the read comes back as exactly the 5-byte `"third"` tail. One trap found the hard way: the *open* must also say `BlobOptions().setType(BlobType::STREAM)` — the stored type does not carry over, so a default open serves the same blob in segmented mode, where the first read chunks at 22 bytes and a `seek()` succeeds but the next read returns garbage past the logical remainder (reproduced against the raw OO API too — remote protocol only, embedded is correct — and reported upstream as [firebird#9101](https://github.com/FirebirdSQL/firebird/issues/9101)).

### JavaScript sample — [`samples/nodejs/blobs.js`](samples/nodejs/blobs.js)

node-firebird's blob story is streaming-shaped at both ends (`cd samples/nodejs && node blobs.js`): a string or `Buffer` *parameter* is automatically turned into `op_create_blob2` plus 1 KB segment batches, and a blob *column* comes back as a **function** that yields an `EventEmitter` of data chunks — the driver's `op_open_blob`/`op_get_segment` loop surfacing in the API. Verified output for a 4,950-byte text and a 256-byte binary blob:

```text
wrote: 4950-byte text blob, 256-byte binary blob
note (segment stream): 5 chunk(s) [1022, 1020, 1020, 1020, 868] = 4950 bytes
data (segment stream): 1 chunk(s) [256] = 256 bytes
binary round-trip intact: true
BLOB_APPEND: 17 bytes -> "part1-part2-part3"
done.
```

The five ~1 KB chunks are the driver's own write-side segmentation coming back to it. One honest limitation: node-firebird 2.x exposes `maxInlineBlobSize` for the [FB5 inline-blob wire optimization](#segmented-and-stream-access) (`op_inline_blob`), but enabling it against this Firebird 6 server hangs the query — a driver/protocol-19 issue, so the sample sticks to the classic segment stream.

### Rust sample — [`samples/rust/src/bin/blobs.rs`](samples/rust/src/bin/blobs.rs)

The Rust version through [rsfbclient](https://github.com/fernandobatels/rsfbclient), Rust's Firebird client (`cd samples/rust && cargo run --bin blobs`), sits at the opposite extreme from both neighbours: where C++ hand-drives `putSegment`/`getSegment` and node-firebird at least surfaces the read side as a chunk stream, rsfbclient makes a blob simply a whole Rust value — a `String` or `Vec<u8>` bound like any other parameter. The segmented API still runs underneath, with rules worth knowing: a `String` parameter up to 32767 bytes travels as VARCHAR and the *server* coerces it into the blob column; above that the driver switches to `isc_create_blob` + `isc_put_segment` in 64K chunks; a `Vec<u8>` parameter always takes the blob path regardless of size. On fetch the driver drains `isc_get_segment` 255 bytes at a time into one buffer. Segment boundaries, `getInfo` statistics (segment count, longest segment, segmented vs stream), blob seek and BPB options are all invisible — the honest cost of the whole-value model. The catalog view of subtypes and `BLOB_APPEND` work exactly as in the other samples.

Verified: the 46-char short text round-trips via the VARCHAR coercion path; the 40000-byte long parameter (`> 32767`) becomes a driver-created blob reporting `40000 octets / 40000 chars on the server`; the 1000-byte binary `Vec<u8>` reads back `identical: true`; `RDB$FIELDS` shows `DATA` subtype 0 with charset `<null>` and `NOTE` subtype 1 `UTF8`; and `BLOB_APPEND` yields `17 octets, 17 chars, content "part1-part2-part3"` — the same 17 bytes the JavaScript run prints.

### Free Pascal sample — [`samples/fpc/blobs.pas`](samples/fpc/blobs.pas)

The same scenario through [fbintf](https://github.com/MWASoftware/fbintf) (vendored at [`extern/fbintf`](extern/fbintf)), MWA Software's Firebird Pascal API — the layer under IBX (`make -C samples/fpc bin/blobs && samples/fpc/bin/blobs`) — which sits between the C++ hand-driven segments and Rust's whole-value model: `Att.CreateBlob(Tr, 'DOCS', 'NOTE')` returns an `IBlob`, each `IBlob.Write` call up to 64 KB is exactly one `putSegment` underneath, and `SQLParams[1].AsBlob := Blob` closes the blob and binds its 8-byte quad id into the row. Uniquely among the wrappers here, the blob's [statistics](#multi-level-page-addressing) stay reachable: one typed `IBlob.GetInfo` call returns segment count, longest segment, total length and segmented-vs-stream type, where the JS and Rust drivers surface none of that. Two honest quirks in the other direction: `IBlob.Read` is stream-style, coalescing segments into the caller's buffer instead of stopping at segment boundaries the way raw `getSegment` does, and `IB.pas` declares no Seek method at all — stream-blob seeking sits below fbintf's veneer. The catalog subtype query and `BLOB_APPEND` close the loop, with `ISQLData.AsString` reading a whole blob column without any cast.

Verified: the three `IBlob.Write` calls come back in a single `IBlob.Read` of 40 bytes (`"first segmentsecond, longer segmentthird"`), while `GetInfo` still proves the on-disk structure — `3 segments, longest 22, total 40 bytes, type 0 (0=segmented)`; `RDB$FIELDS` shows `DATA` subtype 0 charset `<null>` and `NOTE` subtype 1 `UTF8`; `BLOB_APPEND` yields `17` octets, `17` chars, `part1-part2-part3` — the same 17 bytes as the other three runs.

### Things to try

- Grow the C++ text blob (e.g. 64 putSegment calls of 4 KB) and watch `getInfo` report the level change indirectly: re-run `gstat -r` on the table and see blob pages appear, then compare `Average record length` — it stays ~15 bytes no matter how big the blobs get.
- Open the text blob with a BPB requesting charset conversion (`isc_bpb_target_type`/`isc_bpb_target_interpretation`) and watch UTF8 text arrive transliterated — the [filter/charset machinery](#subtypes-charsets-and-filters) in action.
- In the JS sample, set `blobChunkSize: 4096` in the attach options and watch the read-side chunk sizes change to match — the segments you write are the segments you read.
- Replace `BLOB_APPEND` with plain `note || 'suffix'` concatenation in a loop and compare timings as the blob grows — the recopying cost `BLOB_APPEND` exists to avoid.

### Debugging this in C++ (gdb)

With a [debug build of the engine](debugging-firebird.md), the blob path is compact and very watchable:

```gdb
break blb::create2          # src/jrd/blb.cpp:257  — a blob being created (BPB parsed here)
break blb::open2            # src/jrd/blb.cpp:1328 — open for reading; blob id -> blh lookup
break blb::BLB_put_segment  # src/jrd/blb.cpp:1589 — one segment in; watch blb_level promotion
break blb::BLB_get_segment  # src/jrd/blb.cpp:662  — one segment out
break blb::move             # src/jrd/blb.cpp:1003 — temp blob materialized into a record field
```

`BLB_put_segment` fires once per `putSegment()` from the C++ sample (three times) and once per 1 KB chunk from the JS one; inside, the `blb` object's `blb_level`, `blb_count` and `blb_max_segment` members are the live form of the on-disk `blh` header, and stepping through a large write shows the [level promotion](#multi-level-page-addressing) happen when the page list overflows. `blb::move` is where a `BLOB_APPEND` temp blob gets **materialized** — the backtrace runs from the INSERT's `VIO_store` down into blob copy, showing why temp blob ids are transient. See the [debugging guide](debugging-firebird.md) for running the engine embedded under gdb.

## Further research

**Firebird**

- [`doc/sql.extensions/README.blob_append.md`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.blob_append.md) and [`README.blob_util.md`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.blob_util.md) — the FB4/FB5 BLOB manipulation functions; [`src/jrd/blb.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/blb.cpp) — the BLOB engine (levels, segments).
- The [on-disk structure document](on-disk-structure.md) (blob pages, `gstat`), [SQL dialect and data types](sql-dialect-and-types.md) (BLOB as a type), [internationalization](internationalization.md) (text-blob charsets), and the [wire-protocol document](firebird-wire-protocol.md) (blob opcodes, FB5 inline blobs).

**PostgreSQL**

- [Binary data (`bytea`)](https://www.postgresql.org/docs/current/datatype-binary.html), [TOAST](https://www.postgresql.org/docs/current/storage-toast.html), [Large objects](https://www.postgresql.org/docs/current/largeobjects.html).

**MySQL**

- [BLOB and TEXT types](https://dev.mysql.com/doc/refman/8.4/en/blob.html), [InnoDB row formats](https://dev.mysql.com/doc/refman/8.4/en/innodb-row-format.html) (off-page storage); MariaDB's [BLOB](https://mariadb.com/kb/en/blob/).

**SQLite**

- [Datatypes (BLOB)](https://sqlite.org/datatype3.html), [Incremental BLOB I/O](https://sqlite.org/c3ref/blob_open.html), [internal vs external BLOBs](https://sqlite.org/intern-v-extern-blob.html), [limits](https://sqlite.org/limits.html).
