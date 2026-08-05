# fire-crab: Converting the Firebird Engine to Rust

This collection now has a practical continuation:
[**fire-crab**](https://github.com/mariuz/fire-crab) (vendored at
[`extern/fire-crab`](extern/fire-crab)) — an incremental conversion of the
Firebird engine from C++ to Rust that uses these forty-three documents as its
guidebook and their hands-on samples as its test oracles.

## Why a conversion, and why from this paper

The engine is on the order of a million lines of C++; a big-bang rewrite of
such a system loses precisely the thing that makes Firebird valuable — thirty
years of accumulated correctness. fire-crab therefore inverts the usual
rewrite pitch. Its rules:

1. **Every converted piece is verifiable against the C++ engine today.** The
   first slice decodes real database files and is diffed field-for-field
   against `gstat` — currently passing on all **123 scratch databases** that
   this paper's hands-on samples generate.
2. **The C++ source is the specification**, and this paper is the map to it.
   Each subsystem document here explains what a piece of the engine *does*;
   the conversion reads the document first, then the C++, then writes Rust
   with the source file:line cited and the C++ `static_assert`s mirrored as
   Rust tests.
3. **The five language families of samples are free test vectors.** The BLR
   samples' byte dumps, the transactions samples' conflict chains, the
   on-disk samples' header values — all become expected outputs for
   differential tests as the conversion reaches each subsystem.

## What exists today

The storage layer, bottom-up ([status table](https://github.com/mariuz/fire-crab#status)):

- `fire-crab-ods` — the generic page header, database header page and
  transaction inventory pages converted from
  [`src/jrd/ods.h`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/ods.h)
  (the layouts this paper's [on-disk-structure document](on-disk-structure.md)
  walks through), plus the record RLE codec from
  [`src/jrd/sqz.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/sqz.cpp)
  including the Firebird 4+ extended run forms.
- `fcstat` — a `gstat`-like inspector: `header`, `census`, `tip` — the
  conversion's observable face and the vehicle for the differential QA
  (`qa/diff-gstat.sh`) and the C++-vs-Rust benchmark (`bench/compare.sh`).

QA and benchmark results, with their caveats spelled out, live in
[fire-crab's qa-and-benchmarks document](https://github.com/mariuz/fire-crab/blob/master/docs/qa-and-benchmarks.md);
the short version: header decode agrees with `gstat` on every compared field
across every database this paper can produce; the record walk now decodes
full rows from raw pages — RDB$FORMATS bootstrap, blob-id resolution,
segmented blob assembly, descriptor-driven field decode — matching live
`SELECT` output value-for-value on every compared column, from empty tables
through blob-bearing ones to a 200,000-row relation, with the not-yet-decoded
types (DECFLOAT, INT128, TZ) excluded per-table and visibly, never silently; tool wall-clock is the same
order of magnitude (1.8 ms vs 2.6 ms, both startup-dominated), and the
official [firebird-qa](https://github.com/FirebirdSQL/firebird-qa) suite is
explicitly the *milestone* — now in reach rather than distant: fire-crab
speaks the *server* half of the wire protocol well enough that a genuine
third-party client (node-firebird) authenticates via SRP-256, encrypts the
wire and runs real queries end-to-end — column projections (`SELECT <cols>` /
`SELECT *`), `WHERE` filtering (comparisons, `AND`/`OR`, `IS [NOT] NULL`,
three-valued logic), `ORDER BY` (columns/ordinals, ASC/DESC, engine NULL
ordering), `MIN`/`MAX`/`SUM`/`COUNT` aggregates, `GROUP BY` (grouped
aggregates with NULL keys bucketing together, multi-aggregate global queries
answering one row even over an empty set), `HAVING` (filtering the
computed groups, including by aggregates not in the select list) and INNER
and LEFT/RIGHT/FULL OUTER joins (two relations matched on AND-ed column
equalities through qualified or bare names, NULL keys never joining —
outer kinds NULL-padding the partnerless rows, with WHERE evaluated on the
padded row so `WHERE right IS NULL` is the classic anti-join) — with every
exactly-decoded column
type travelling in the engine's native wire form (integers at their own
width, scaled numerics as raw ints + describe scale, float/double,
date/time/timestamp in raw units, boolean, INT128 and DECFLOAT — the
latter decoded from the densely-packed-decimal encoding via the engine's
own decNumber tables and rendered exactly as `decNumberToString` does,
cohort and all — and the TIME ZONE types, offset zones converted to local
exactly as `TimeZoneUtil` does and named zones rendered with the right
region name but visibly unconverted (their rules need ICU's tzdata; a
marked placeholder is honest where a wrong local time would not be);
blob columns served as real
blobs — the id in the row, the content through the blob ops, matching isql
on multi-page and system blobs alike) — and DML now covers all three
verbs: `INSERT` writes real records into the pages (transaction allocated
and committed in the TIP, image laid into a data-page slot), and
`UPDATE`/`DELETE` write real MVCC *version chains* — the old version copied
to a fresh slot flagged `rhd_chain`, the primary rewritten with a back
pointer (a patched image for update, a deleted stub for delete) — and the
write path now ALLOCATES AND MAINTAINS INDEXES: when every data page is
full the relation grows (pages from the PIP, the file extending past EOF,
pointer pages hooked up), and DML on indexed tables writes real B-tree
entries — keys byte-exact per btr.cpp's `compress`, nodes in btn.h's
prefix-compressed encoding, page splits and root splits per
`split_and_insert` — that the ENGINE then reads through: isql point
lookups with PLAN-asserted index scans, range scans, navigational ORDER
BY, all finding exactly the rows fire-crab wrote, with `gfix -v -full`
cross-checking every record against every index entry and duplicate
primary keys refused — and the index shapes real schemas are full of:
compound keys stuffed into marker-interleaved groups exactly as
`BTR_key` assembles them, DESCENDING keys complemented whole after
assembly, with the unique rule the LIVE ENGINE corrected mid-conversion
(only an all-NULL key is exempt from uniqueness, btr.cpp:5629 — the
first draft's any-NULL exemption was refused by the engine itself;
expression/conditional indexes still refuse DML) — with the
engine itself as the oracle three ways: `gfix -v -full` accepts the chains,
the engine applying the *same statements* to a second copy produces an
identical table, and `gfix -sweep` — the engine's own garbage collector —
consumes the chains fire-crab wrote, the version count dropping exactly as
arithmetic predicts with the data intact — the
server opening the attached
file, resolving table and columns through
`RDB$RELATIONS`/`RDB$RELATION_FIELDS`, decoding typed rows from the pages and
evaluating/grouping/sorting/accumulating — matching isql value-for-value on
user tables (including NULLs and mixed-width tables, where the record-format
field id diverges from the column position) *and on the system catalog
itself*: system relations' formats, absent from `RDB$FORMATS`, are computed
by the engine's own bootstrap walk ([ini.epp](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/ini.epp)'s
`FLAG_BYTES` + `MET_align` offset computation) over the database's own
catalog rows — the file describes itself, and the computation must
reproduce the offsets earlier increments established by differential
testing before it is trusted. Prepared statements now take real `?`
parameters — the describe's bind section announces each parameter's
target type (the metadata real drivers build their encoders from), the
op_execute message is decoded from the client's own value-derived BLR,
and the values bind into INSERT images, SET bytes and WHERE comparisons
with the engine's CVT coercions (integers rescaling exactly into
NUMERIC, doubles rounding half-away, timestamps truncating into
DATE/TIME), with NULL-parameter comparisons correctly UNKNOWN and
type mismatches raising SQL errors, the engine printing the identical
table from the literal equivalents. The predicate surface is filled
in: `LIKE` (with `ESCAPE`), `BETWEEN`, `IN`, `NOT` and parentheses run
through a recursive parser normalized to DNF with De Morgan push-down
(sound in three-valued logic — `x NOT IN (10, NULL)` correctly returns
nothing), LIKE matching the stored value per character with CHAR
padding counting, every shape probed against the engine before
implementation and gated by running the identical SQL through both
sides. And the first DDL is converted: CREATE TABLE writes the catalog
itself — rows in five system relations with their indexes maintained,
the format-descriptor and RDB$RUNTIME blobs byte-pinned by probing
engine-created databases, the pointer and index-root pages allocated
and registered — and the ENGINE then adopts the table: reads it,
inserts into it, creates its own table beside it, gbak-restores the
file (replaying our catalog as its own DDL) and drops it through our
rows, `gfix -v -full` clean throughout. The hunt for a hung attach
ended in the submodule's DEBUG engine build, whose assert named the
law we broke — RDB$PAGES rows must be system-transaction records —
the differential method reaching all the way into the engine's own
debugger. And the constraints followed: PRIMARY KEY and NOT NULL
(the engine's own duplicate INSERT dies naming the INTEG_n constraint
row fire-crab stored, its NOT NULL validation fires off the
runtime-blob segment fire-crab wrote), CREATE INDEX with backfill
(the engine PLAN-navigates it; a unique backfill over duplicate data
fails like the engine's own index build), and DROP TABLE (catalog
rows stubbed for the engine's sweep to collect, pages released, the
restored backup still enforcing the PK). And the suite's own client
now works against it: the reference python firebird-driver — the OO
client firebird-qa's tests run every SQL statement through — drives
fire-crab end-to-end (SELECT, parameterised DML, CREATE TABLE with a
primary key the driver's own duplicate insert then hits, DROP TABLE,
a reconnect reading the committed writes), which forced the protocol
to answer the client's exact requested info-item template and to echo
the transaction handle the way the engine does. **And the suite now runs.** The official firebird-qa pytest suite,
pointed at `fcwire serve`, bootstraps fully — its banner reads
`server: fc [v6.0.0.2076, Embedded, Firebird/Linux/ARM64]`, ODS 14.0 —
collects the basic functional tests, runs them, and *seven to nine of
them pass*: real tests from the real suite, green against a
bottom-up Rust reimplementation of the storage engine. Reaching
"tests pass" from the cleared session bootstrap took the Services API
and `op_create` (converted last increment), then a handful of
info-and-monitoring pieces: `op_info_database` breadth so
`con.info.name`/`.id`/`.ods_version` answer, a distinct attachment id
per connection, the `MON$ATTACHMENTS` virtual table reported as empty
(fire-crab keeps no live monitoring state, so the architecture probe
returns one all-NULL row and classifies it as an embedded server), and
the service RUNNING info item so the isql-test fixtures' forced-writes
step completes. The passing tests drive the whole stack — the real
isql binary through fbclient, the python driver creating and querying
databases, fire-crab serving and mutating the files, the engine
validating them. This is no longer a milestone in prospect: firebird-qa
*runs* against fire-crab, and the count of passing tests is now a number
that grows with the SQL surface (the current failures are `SHOW
DATABASE` and other surface gaps, and version- or architecture-gated
skips — not the storage engine, the indexes, the statement protocol,
the catalog, the constraints, the handshake or the session bootstrap,
all of which the real suite now exercises end to end). That growth has
started: the SELECT list now takes scalar expressions — arithmetic over
integer columns and literals, evaluated per row with NULL propagating,
each shape checked identically against isql down to the engine's own
column names (`ADD`/`SUBTRACT`/`MULTIPLY`) — and `op_exec_immediate` was
added for isql's one-shot SET/DDL path; the `SHOW` commands remain,
running through the legacy BLR request API (`op_compile`) rather than
DSQL — a named next piece.

## Conversion pointers: document → C++ → Rust

The full five-phase map is
[fire-crab's subsystem-map](https://github.com/mariuz/fire-crab/blob/master/docs/subsystem-map.md);
the reading order for anyone joining the effort:

| To convert... | Read here first | Then the C++ | Status |
|---|---|---|---|
| Pages, records, RLE | [on-disk-structure.md](on-disk-structure.md) | `src/jrd/ods.h`, `sqz.cpp` | **done** |
| PIP / pointer / data pages, record walk | [on-disk-structure.md](on-disk-structure.md), [transactions-and-concurrency.md](transactions-and-concurrency.md) | `ods.h`, `dpm.cpp` | **done** — the record walk re-derives `SELECT COUNT(*)` from raw pages, verified against the live engine from 0 to 200k rows |
| Record field decoding | [on-disk-structure.md](on-disk-structure.md), [metadata-cache.md](metadata-cache.md), [catalog-bootstrap.md](catalog-bootstrap.md) | RDB$FORMATS blobs, `met.epp` | **done** — full rows decoded from raw pages via the same metadata bootstrap the engine uses, matching live `SELECT` value-for-value |
| B-tree pages | [indexing-and-full-text-search.md](indexing-and-full-text-search.md) | `btr.cpp`, `btn.h` | **done** — leaf-level walks with prefix decompression match the engine's `ORDER BY` over the same index, row-for-row at 200k rows |
| Transactions, TIP semantics, snapshots | [transactions-and-concurrency.md](transactions-and-concurrency.md) | `tra.cpp`, `vio.cpp`, `sqz.cpp` | **done** — TIP-driven committed-only visibility walk (with delta reconstruction) matches a fresh-snapshot `SELECT` on a file frozen mid-uncommitted-work |
| Record versions and GC | [garbage-collection-and-sweep.md](garbage-collection-and-sweep.md) | `vio.cpp` | **done** — predicts which versions a sweep collects; the prediction matches `gfix -sweep`'s actual removal exactly (210 versions across both GC paths) |
| Page cache and careful writes | [page-cache-coherency.md](page-cache-coherency.md), [careful-writes-and-crash-safety.md](careful-writes-and-crash-safety.md) | `cch.cpp` | **wired** — the server flushes in the precedence order (every write prefix is a database the engine can open), and the pages of a file live **once per process** in the buffer pool rather than once per attachment, with writers serialized over them: two attachments now see each other's committed rows and neither loses the other's. Per-page fetch and eviction are still to come |
| Lock manager | [lock-manager.md](lock-manager.md) | `src/lock/lock.cpp` | converted, not wired — the lock table decodes cell-for-cell against `fb_lock_print`; what it would add is *granularity*, since the server currently blocks a second writer on the whole database rather than on a conflicting row |
| BLR decode | [blr-intermediate-language.md](blr-intermediate-language.md) | `par.cpp`, `blp.h`, `gds.cpp` | **done** — 171-verb walker; every decodable BLR blob matches the engine's own `SET BLOB ALL` printer token-for-token |
| DSQL, execution, optimizer | [grammar-and-parser.md](grammar-and-parser.md), [query-optimizer-and-execution.md](query-optimizer-and-execution.md) | `src/dsql/`, `exe.cpp` | planned |
| Wire protocol — client (`src/remote/`, `src/auth/`) | [firebird-wire-protocol.md](firebird-wire-protocol.md), [security-architecture.md](security-architecture.md) | `src/remote/`, `src/auth/` | **fire-crab runs general SELECTs** — login (SRP-256/Arc4/attach) plus prepare/execute/batched-fetch of integer+text columns, matching isql row-for-row. This is a wire *client* that validates the codec against the real engine |
| Wire protocol — server (the firebird-qa milestone) | [firebird-wire-protocol.md](firebird-wire-protocol.md) | `src/remote/` server side | **accepts real clients and answers real filtered/sorted/aggregated queries** — a third-party driver (node-firebird) negotiates protocol 20, authenticates via the *server* half of SRP-256, arms Arc4 encryption, and runs column projections (`SELECT <cols>` / `SELECT *`), `WHERE` filtering (comparisons, `AND`/`OR`, `IS [NOT] NULL`, three-valued logic), `ORDER BY` (columns/ordinals, ASC/DESC, engine NULL ordering), `MIN`/`MAX`/`SUM`/`COUNT` aggregates, `GROUP BY` (grouped aggregates, NULL keys bucketing together, multi-aggregate global queries) and `HAVING` (evaluated on the computed group rows, aggregates in the predicate resolved to output items — hidden ones when not selected) and INNER + LEFT/RIGHT/FULL OUTER equi-joins (qualified/bare column resolution, multi-key `ON`, NULL keys never matching, pad-insensitive text keys; outer kinds NULL-pad partnerless rows and WHERE runs on the padded row — the anti-join) end-to-end — returning native wire types (SHORT/LONG/INT64, scaled numerics the client divides per the describe, IEEE float/double, raw-unit date/time/timestamp, boolean, INT128 and DECFLOAT(16/34) — DPD-decoded via the engine's own decNumber tables, rendered per decNumberToString with cohort preserved, serialized exactly as xdr.cpp does — the TIME ZONE types (UTC + zone id, offset zones converted exactly, named zones honest, ORDER BY by UTC instant), and blobs served as real blobs (the 8-byte id in the row, content through `op_open_blob`/`op_get_segment`, first content-level blob differential incl. a multi-page level-1 blob and a system blob): the server opens the attached file, resolves table and columns through `RDB$RELATIONS`/`RDB$RELATION_FIELDS`, decodes records from the pages, evaluates/groups/sorts/accumulates, and returns typed rows matching isql value-for-value on user tables (incl. NULLs and mixed-width tables where field id ≠ column position) AND on system relations — their formats, absent from `RDB$FORMATS`, computed from the database's own catalog by the ini.epp bootstrap walk, anchored to the differentially-established offsets before being trusted. DML covers all three verbs: INSERT writes real records into the pages, UPDATE/DELETE write real version chains (old version copied out as `rhd_chain`, primary rewritten with a back pointer — a deleted stub for DELETE), with the engine as oracle three ways — isql prints the identical table the engine itself produces from the same statements, `gfix -v -full` accepts the chains, and `gfix -sweep` (the engine's own GC) collects them exactly. The write path is complete for the common shapes — records, version chains, page allocation and B-tree index maintenance all engine-validated — and prepared statements take real `?` parameters: the bind section describes each target, the client's value-derived input BLR is decoded, values bind with engine-CVT coercions (NULL params = UNKNOWN, mismatches = SQL errors), all driven by node-firebird's genuine encoders with the engine printing the identical table from literal equivalents; what stands before firebird-qa runs is breadth (select-list expressions/arithmetic, ALTER TABLE, foreign keys; the WHERE predicate surface is done, compound + descending indexes maintained, and CREATE TABLE/PK/NOT NULL/CREATE INDEX/DROP TABLE are engine-adopted and engine-ENFORCED — the engine refuses duplicate keys through fire-crab's indexes, naming fire-crab's constraint rows) |
| Services, events, security | [services-api.md](services-api.md), [firebird-events.md](firebird-events.md), [security-architecture.md](security-architecture.md) | `svc.cpp`, `event.cpp`, `src/auth/` | planned |

The conversion's working rules (explicit little-endian decoding instead of
struct overlay casts, `Option`/`Result` instead of zero-returns, the
"convert behaviour, not style" principle, the no-`unsafe` and no-async ground
rules) are documented with their rationale in
[fire-crab's methodology](https://github.com/mariuz/fire-crab/blob/master/docs/methodology.md) —
including what the first slice taught the hard way (benchmark metrics that
flatter, tools that must do comparable work to be comparable).

## Trying it

```sh
git submodule update --init extern/fire-crab
cd extern/fire-crab
cargo test && cargo build --release
./target/release/fcstat header /tmp/fbhandson/monitoring_fbcpp.fdb
GSTAT=/opt/firebird/bin/gstat qa/diff-gstat.sh /tmp/fbhandson/*.fdb
```

Any database created by this paper's hands-on samples is a test vector; the
`fcstat header` output should agree with `gstat -h` on every compared field,
and a `DIFF` line from the QA script is, by construction, a conversion bug
worth filing.
