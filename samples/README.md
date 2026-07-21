# Samples

Runnable C++ and JavaScript examples that exercise the architecture
described in the [paper](../README.md). The C++ programs use the modern
object-oriented client API (`firebird/Interface.h`) that is the supported
interface in Firebird 3 and later, including the Firebird 6 development
branch; the JavaScript programs use [node-firebird](https://github.com/hgourvest/node-firebird),
a pure-JS implementation of the wire protocol.

## Per-document hands-on samples

Every subsystem companion document ends with a **"Hands-on: samples, tests
and debugging"** section built on a pair of samples here:

- **`cpp/<topic>.cpp`** — one focused OO-API program per document
  (`cpp/transactions_demo.cpp` for the transactions document,
  `cpp/lock_manager.cpp` for the lock manager, and so on), all sharing the
  boilerplate in [`cpp/fb_sample.h`](cpp/fb_sample.h) (RAII attachment,
  TPB builder, fetch-everything-as-text queries). The CMake build below
  compiles each to a binary of the same name, always with `-g` so the gdb
  walk-throughs in the documents work out of the box.
- **`fb-cpp/<topic>.cpp`** — the same program a third time, written against
  [fb-cpp](https://github.com/asfernandes/fb-cpp) (vendored at
  [`../extern/fb-cpp`](../extern/fb-cpp)), the modern C++20 wrapper over
  the OO API: RAII attachments and transactions, `std::optional` for
  nullable values, typed builder options where the OO API takes parameter
  blocks, `Boost.Multiprecision` for INT128/DECFLOAT. Each twin's header
  comment names the instructive diff against its OO-API sibling — what the
  wrapper absorbs (TPB/DPB/SPB bytes, status-vector plumbing, message
  buffers) and where it hands back the raw interface (`getHandle()` for
  service actions it does not wrap). Built as `fbcpp_<topic>` by the same
  CMake run when Boost is installed; they share only the tiny
  [`fb-cpp/fbcpp_sample.h`](fb-cpp/fbcpp_sample.h) (credentials + error
  reporting), because the wrapper leaves almost no boilerplate to share.
- **`nodejs/<topic>.js`** — the JavaScript twin, sharing
  [`nodejs/common.js`](nodejs/common.js) (promisified attach/query/
  transaction, raw-TPB isolation constants). Where the driver cannot reach
  a feature the document says so honestly — and those deltas (type-mapping
  gaps, Arc4-vs-ChaCha wire crypt, WAIT-vs-NO WAIT defaults) are themselves
  part of what the samples teach.
- **`fpc/<topic>.pas`** — the Free Pascal twin, written against
  [fbintf](https://github.com/MWASoftware/fbintf) (vendored at
  [`../extern/fbintf`](../extern/fbintf)), MWA Software's Firebird Pascal
  API — the layer under IBX — driving the same libfbclient as the C++
  samples behind COM-style reference-counted interfaces. It is the
  richest wrapper of the twin families: a real Services API
  (`IServiceManager` runs the verbose gbak backup and the full
  two-service trace session natively), events with delivered counts,
  `GetPlan` (always the detailed form), INT128/DECFLOAT as `TBCD`,
  time-zone decoding down to the zone name, and TPB-level table
  reservation — most of the gaps the JavaScript and Rust twins declare
  simply do not exist here. Its own quirks are documented where they
  bite: no blob `Seek`, status-vector warnings discarded, a DECFLOAT
  decode bug for scales 16–31, and the sample for the types document is
  named `types_demo.pas` because a program named `types.pas` would
  shadow the FPC RTL unit and break every build in the directory.
- **`rust/src/bin/<topic>.rs`** — the Rust twin, written against
  [rsfbclient](https://github.com/fernandobatels/rsfbclient) (one cargo
  package, one binary per topic, shared helper in
  [`rust/src/lib.rs`](rust/src/lib.rs)). rsfbclient speaks two dialects —
  the NATIVE backend drives the same libfbclient as the C++ samples, and
  the PURE RUST backend is an independent wire-protocol implementation
  (protocol 13, Srp256, Arc4 — the protocol samples contrast both) — plus
  embedded mode (`with_embedded()`), which several twins use for
  crash-safety and cache-topology experiments. Its deltas teach too:
  transactions are typed `TransactionConfiguration` objects but the
  connection's hidden default transaction is commit-RETAINING; the type
  mapping is coarse (all integers → `i64`, NUMERIC → lossy `f64`, no
  INT128/DECFLOAT/TIMESTAMP-TZ — CAST to VARCHAR or lose precision); there
  is no Services API (the trace twin is the only one that cannot run its
  document's demonstration at all) and no plan API (the twins use Firebird
  6's `RDB$SQL.EXPLAIN` instead).

Each document's Hands-on section shows its pair's *verified* output, a
"things to try" list, and gdb breakpoints into the engine functions the
document discusses — see the [debugging guide](../debugging-firebird.md)
for the setup they assume. The samples default to the demo server
(`inet://localhost/employee`, SYSDBA/masterkey) or to scratch databases
under `/tmp/fbhandson/` (create it once: `mkdir -p /tmp/fbhandson && chmod
777 /tmp/fbhandson` — the server writes there as its own user).

## Original walk-through samples

- **`client_test.cpp`** — a complete client round-trip through the
  top-level architecture of Figure 1: the call chain goes from the client
  through the **REMOTE** subsystem and the **Y-valve** dispatcher into
  **DSQL** (SQL → BLR translation) and the **JRD** engine. It attaches to
  (or creates) a database, recreates a table, inserts rows inside a
  transaction, and reads them back through a cursor with coerced output
  metadata.

- **`protocol_client.cpp`** — a networked OO-API client that attaches over
  `inet://` and asks the engine which protocol, authentication plugin and
  wire-encryption were negotiated. It is the C++ counterpart to
  `nodejs/query.js` and is discussed in
  [../firebird-wire-protocol.md](../firebird-wire-protocol.md).

- **`events_demo.cpp`** — the event-notification round trip: a listener
  attachment registers `'demo_event'` with `IAttachment::queEvents()`
  (opening the auxiliary connection) while a poster attachment runs
  `POST_EVENT` blocks, proving the three defining semantics — rollback
  swallows posts, delivery happens at commit, and same-name posts in one
  transaction coalesce into a count. Set `EVENTS_DEMO_PAUSE_MS` to hold
  the listener open and observe the extra TCP connection. Discussed in
  [../firebird-events.md](../firebird-events.md).

- **`nodejs/`** — the same connection from Node.js, two ways:
  `query.js` uses the pure-JavaScript **node-firebird** driver, and
  `srp-handshake.js` re-implements the wire protocol and the Srp256/Arc4
  handshake from scratch with no dependencies. See
  [../firebird-wire-protocol.md](../firebird-wire-protocol.md).

## Prerequisites

- A C++17 compiler (C++20 for the `fb-cpp/` twins).
- The Firebird API headers, provided by the `extern/firebird` submodule —
  and, for the `fb-cpp/` twins, the vendored fb-cpp sources plus the Boost
  headers (fb-cpp loads `libfbclient` at runtime through Boost.DLL, so
  those binaries never link it):

  ```sh
  git submodule update --init extern/firebird extern/fb-cpp
  sudo apt install libboost-dev libboost-filesystem-dev   # Debian/Ubuntu
  ```

  Without Boost or the submodule the CMake run simply skips the fb-cpp
  twins with a warning; everything else still builds.

- The Firebird client library and a server to talk to:

  ```sh
  # Debian/Ubuntu
  sudo apt install firebird-dev firebird3.0-server
  # Fedora
  sudo dnf install firebird-devel firebird
  ```

## Building

With CMake:

```sh
cmake -B build samples
cmake --build build
```

Or directly:

```sh
c++ -std=c++17 -I extern/firebird/src/include samples/client_test.cpp \
    -lfbclient -o client_test
```

## Running

```sh
./client_test [database]
```

The database path defaults to `employees.fdb` in the current directory; a
remote string such as `localhost:/data/employees.fdb` also works.
Credentials are taken from the `ISC_USER` / `ISC_PASSWORD` environment
variables, defaulting to `SYSDBA` / `masterkey`.

Expected output:

```text
Created new database: employees.fdb
Table 'people' ready.
Inserted 4 rows.

ID   NAME                 CITY
---- -------------------- --------------------
1    Ada Lovelace         London
2    Grace Hopper         New York
3    Edsger Dijkstra      Rotterdam
4    Jim Starkey          Manchester

Done.
```

### `protocol_client` (networked)

Attaches over TCP and reports the negotiated protocol:

```sh
./protocol_client inet://localhost/employee SYSDBA masterkey
```

```text
attached to inet://localhost/employee
engine version : 6.0.0
protocol       : TCPv4
wire crypt     : ChaCha64
authenticated  : SYSDBA
detached. bye
```

## Node.js samples

The [`nodejs/`](nodejs/) directory contains the same connection from
JavaScript. Install the driver and run:

```sh
cd samples/nodejs
npm install                 # pulls node-firebird
node query.js               # high-level driver
node srp-handshake.js       # from-scratch wire protocol + SRP + Arc4
```

`query.js` reports the negotiated connection just like `protocol_client`
(note that node-firebird negotiates **Arc4** where the C++ client, using
fbclient's default plugin order, negotiates **ChaCha64**). `srp-handshake.js`
prints every SRP intermediate value and every deviation from the SRP RFCs;
its full annotated walk-through is
[../firebird-wire-protocol.md](../firebird-wire-protocol.md).

The per-document twins run the same way once the driver is installed:

```sh
cd samples/nodejs
node transactions.js        # or any other <topic>.js
```

## Free Pascal samples

The [`fpc/`](fpc/) directory holds the per-document twins in Free Pascal.
With fpc 3.2+ (`sudo apt install fpc`), the Firebird client library, and
the vendored fbintf submodule:

```sh
git submodule update --init extern/fbintf
make -C samples/fpc                    # everything into fpc/bin/
samples/fpc/bin/transactions           # or any other topic
```

## Rust samples

The [`rust/`](rust/) directory holds the same per-document twins in Rust.
With a Rust toolchain (rustup) and the Firebird client library installed:

```sh
cd samples/rust
cargo run --bin transactions   # or any other <topic>
```

The first build compiles rsfbclient from crates.io; the `linking` feature
links `libfbclient` at build time, so `firebird-dev` (or an equivalent
client install) must be present. Scratch databases go to
`/tmp/fbhandson/<topic>_rust.fdb` like the other twins.

## Debugging

All C++ samples are built with `-g`; the engine side needs a symbolled
engine, which the vendored submodule builds in minutes. The
[debugging guide](../debugging-firebird.md) covers both, the
embedded-engine gdb workflow the documents' breakpoint lists assume, and
the engine's built-in instrumentation that often beats a debugger.
