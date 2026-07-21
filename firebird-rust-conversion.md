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
ordering), `MIN`/`MAX`/`SUM`/`COUNT` aggregates and `GROUP BY` (grouped
aggregates with NULL keys bucketing together, multi-aggregate global queries
answering one row even over an empty set) — the server opening the attached
file, resolving table and columns through
`RDB$RELATIONS`/`RDB$RELATION_FIELDS`, decoding typed rows from the pages and
evaluating/grouping/sorting/accumulating — matching isql value-for-value on
user tables (including NULLs and mixed-width tables, where the record-format
field id diverges from the column position). What stands between here and
running the suite is the remaining *breadth* of the SQL surface (joins,
`HAVING`, more column types, DML), not the protocol — so firebird-qa remains
a milestone, not yet a current coverage claim.

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
| Page cache and careful writes | [page-cache-coherency.md](page-cache-coherency.md), [careful-writes-and-crash-safety.md](careful-writes-and-crash-safety.md) | `cch.cpp` | planned — the correctness gate |
| Lock manager | [lock-manager.md](lock-manager.md) | `src/lock/lock.cpp` | planned |
| BLR decode | [blr-intermediate-language.md](blr-intermediate-language.md) | `par.cpp`, `blp.h`, `gds.cpp` | **done** — 171-verb walker; every decodable BLR blob matches the engine's own `SET BLOB ALL` printer token-for-token |
| DSQL, execution, optimizer | [grammar-and-parser.md](grammar-and-parser.md), [query-optimizer-and-execution.md](query-optimizer-and-execution.md) | `src/dsql/`, `exe.cpp` | planned |
| Wire protocol — client (`src/remote/`, `src/auth/`) | [firebird-wire-protocol.md](firebird-wire-protocol.md), [security-architecture.md](security-architecture.md) | `src/remote/`, `src/auth/` | **fire-crab runs general SELECTs** — login (SRP-256/Arc4/attach) plus prepare/execute/batched-fetch of integer+text columns, matching isql row-for-row. This is a wire *client* that validates the codec against the real engine |
| Wire protocol — server (the firebird-qa milestone) | [firebird-wire-protocol.md](firebird-wire-protocol.md) | `src/remote/` server side | **accepts real clients and answers real filtered/sorted/aggregated queries** — a third-party driver (node-firebird) negotiates protocol 20, authenticates via the *server* half of SRP-256, arms Arc4 encryption, and runs column projections (`SELECT <cols>` / `SELECT *`), `WHERE` filtering (comparisons, `AND`/`OR`, `IS [NOT] NULL`, three-valued logic), `ORDER BY` (columns/ordinals, ASC/DESC, engine NULL ordering), `MIN`/`MAX`/`SUM`/`COUNT` aggregates and `GROUP BY` (grouped aggregates, NULL keys bucketing together, multi-aggregate global queries) end-to-end: the server opens the attached file, resolves table and columns through `RDB$RELATIONS`/`RDB$RELATION_FIELDS`, decodes records from the pages, evaluates/groups/sorts/accumulates, and returns typed rows matching isql value-for-value on user tables (incl. NULLs and mixed-width tables where field id ≠ column position). Widening the remaining SQL surface (joins, `HAVING`, more types, DML) is what stands before firebird-qa runs |
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
