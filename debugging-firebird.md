# Debugging Firebird in C++: a field guide

*A companion to the [conceptual architecture paper](README.md). Every other document in this collection ends with a "Hands-on" section naming the engine functions that implement its subject; this document is the shared recipe for actually stopping on those functions in a debugger — plus the engine's own, often better, alternatives to a debugger.*

The paper describes subsystems — the y-valve, DSQL, the JRD engine, the cache, the lock manager. A debugger turns that description into something you can watch: put a breakpoint on `TRA_commit` and run `COMMIT`, and the backtrace *is* the architecture diagram. This document covers the three ways to get there, in increasing order of power.

## The obstacle: release binaries are stripped

Distribution builds of Firebird (including the `/opt/firebird` install used for the live demos in these documents) are optimized and stripped:

```text
$ file /opt/firebird/plugins/libEngine14.so
... stripped
```

A stripped engine gives gdb nothing to break on except exported dynamic symbols — and the engine's internals (`VIO_modify`, `CCH_fetch`, `LockManager::enqueue`…) are not exported. So real engine debugging starts with building the engine yourself.

## Step 1: build a debug engine

The [`extern/firebird`](https://github.com/FirebirdSQL/firebird) submodule vendored in this repository is a buildable source tree. On a Debian/Ubuntu-family system:

```sh
sudo apt install autoconf automake libtool g++ make \
                 zlib1g-dev libicu-dev libtommath-dev libtomcrypt-dev libedit-dev

cd extern/firebird
./autogen.sh                     # add --enable-developer for a DEV_BUILD (see below)
make -j$(nproc)
```

Two build flavours matter here (both verified on the small single-CPU machine these documents' demos run on — the default build took about **13 minutes**, so this is not the multi-hour commitment it sounds like):

- The **default build** lands in `gen/Release/firebird/` and — pleasant surprise — its binaries are already compiled `-g` and left **unstripped** (stripping happens at packaging time, not build time). That is enough for breakpoints on any engine function by name, with optimized code's usual caveats (inlined frames, `<optimized out>` variables).
- `--enable-developer` produces a **DEV_BUILD** in `gen/Debug/firebird/`: no optimization, full source-line fidelity, and — more valuable than either — hundreds of `fb_assert` checks and debug-only bookkeeping compiled in (the [threading document](threading-and-synchronization.md) shows one: the latch-leak detector in `thread_db`'s destructor). (Historical note: older instructions say `--enable-debug`; current `configure` silently ignores that spelling — check `configure --help`, and watch for `unrecognized options` in its output.)

Either way the result is a complete, self-contained Firebird root: `bin/`, `plugins/libEngine14.so` (with symbols), `intl/`, `firebird.conf`, `databases.conf`, `security6.fdb`. You do not need `make install` and you do not need to touch the production server: everything below runs the debug engine from its build tree.

## Step 2: the royal road — debug the *embedded* engine

The single most useful fact for debugging Firebird (from the [embedded comparison](embedded-architecture-comparison.md)): **libfbclient is also the embedded engine loader**. Attach to a *local path* instead of an `inet://` URL and the y-valve loads `libEngine14.so` into your process — your client program and the whole engine become one debuggable process, no server, no root, no ptrace ceremony.

Point the process at the debug build with the `FIREBIRD` environment variable, which relocates the engine's root directory (config, plugins, security database):

```sh
FBDEBUG=extern/firebird/gen/Debug/firebird     # or gen/Release/firebird

FIREBIRD=$FBDEBUG \
LD_LIBRARY_PATH=$FBDEBUG/lib \
gdb --args ./build/transactions_demo /tmp/fbhandson/debug.fdb
```

Inside gdb the engine library is loaded on first attach, so set breakpoints as *pending*. This is the verified session, run exactly as above against the [transactions sample](transactions-and-concurrency.md#hands-on-samples-tests-and-debugging):

```gdb
(gdb) set breakpoint pending on
(gdb) break TRA_commit
Function "TRA_commit" not defined.
Breakpoint 1 (TRA_commit) pending.
(gdb) run
...
Thread 1 "transactions_de" hit Breakpoint 1, TRA_commit(Jrd::thread_db*, Jrd::jrd_tra*, bool) ()
   from .../gen/Release/firebird/plugins/libEngine14.so
(gdb) bt
#0  TRA_commit(Jrd::thread_db*, Jrd::jrd_tra*, bool) ()            from .../plugins/libEngine14.so
#1  Jrd::JTransaction::internalCommit(Firebird::CheckStatusWrapper*) ()
#2  Jrd::JTransaction::commit(Firebird::CheckStatusWrapper*) ()
#3  Firebird::ITransactionBaseImpl<...>::cloopcommitDispatcher(...) ()
#4  Why::YTransaction::commit(Firebird::CheckStatusWrapper*) ()    from .../lib/libfbclient.so.2
#5  Firebird::ITransactionBaseImpl<...>::cloopcommitDispatcher(...) ()
#6  Firebird::ITransaction::commit<Firebird::ThrowStatusWrapper> (...)
        at extern/firebird/src/include/firebird/IdlFbInterfaces.h:1334
#7  main (argc=2, argv=...) at samples/cpp/transactions_demo.cpp:42
```

The backtrace walks exactly the layers of the paper's Figure 1, bottom frame up: your `main` → the header-only OO API (`IdlFbInterfaces.h`) → the **y-valve**'s `Why::YTransaction` — note it lives in `libfbclient.so` — crossing into the loaded engine plugin through a `cloop` interface dispatcher → the engine's `JTransaction` facade → the internal `TRA_commit`. The two `cloopcommitDispatcher` frames *are* the plugin ABI boundary the [extensibility document](extensibility.md) describes. Every hands-on section's breakpoint list assumes this setup.

With the `--enable-developer` build the same session gains source lines, argument values and live state (also verified — note the frames `Release` inlined away, `JRD_commit_transaction` and the `commit` shim, reappearing):

```gdb
Thread 1 hit Breakpoint 1, TRA_commit (tdbb=0xffffffffe5a8, transaction=0xffffef820650,
    retaining_flag=false) at .../src/jrd/tra.cpp:434
(gdb) bt 4
#0  TRA_commit (tdbb=..., transaction=..., retaining_flag=false) at src/jrd/tra.cpp:434
#1  commit (tdbb=..., transaction=..., retaining_flag=false)     at src/jrd/jrd.cpp:6880
#2  JRD_commit_transaction (tdbb=..., transaction=...)           at src/jrd/jrd.cpp:9591
#3  Jrd::JTransaction::internalCommit (this=..., user_status=...) at src/jrd/jrd.cpp:2580
(gdb) print transaction->tra_number
$1 = 3
```

Three practical notes:

- **Use a scratch database** (the samples default to `/tmp/fbhandson/…`), never a database the production server has open — the [page-cache coherency document](page-cache-coherency.md) explains why a Super server holds an exclusive file lock that will simply refuse your embedded attach (`isc_already_opened`), which protects you from mistakes here.
- **Embedded still authenticates** — as the OS user, bypassing password checks (see [security](security-architecture.md)); `ISC_USER=SYSDBA` is unnecessary and harmless.
- The engine runs its background threads (cache writer, GC — see [threading](threading-and-synchronization.md)) inside your process: `info threads` in gdb shows the architecture directly.

## Step 3: attaching to a running server

When the behaviour you are chasing only happens against the real server (wire protocol, multi-attachment races), run the *debug server* from the build tree and attach gdb to it:

```sh
FIREBIRD=$FBDEBUG LD_LIBRARY_PATH=$FBDEBUG/lib \
  $FBDEBUG/bin/firebird -p 3060 &          # standalone, no systemd needed

gdb -p $(pgrep -n firebird)
```

Attaching to *any* process you own may still require privilege on hardened kernels (`kernel.yama.ptrace_scope=1` → use `sudo gdb -p`). Two things to know once attached:

- **SuperServer is one process, many threads**: a breakpoint stops *all* attachments, and Firebird's worker threads carry no names (`info threads` shows a wall of `firebird`) — identify the interesting thread by its backtrace, or query `MON$ATTACHMENTS` before attaching. The [threading document](threading-and-synchronization.md#live-measurements) maps what you will see.
- **Do not park a breakpointed server on a shared database**: while stopped it still holds its locks, and other attachments will pile up behind them ([lock manager](lock-manager.md)).

For the production `/opt/firebird` server the honest answer is: you can attach, but stripped binaries reduce gdb to exported symbols and raw addresses — use the engine's own instrumentation instead (below), which is precisely why those facilities exist.

## Debugging your own client code

The client half needs no engine build: the samples compile with `-g` (see [`samples/CMakeLists.txt`](samples/CMakeLists.txt)), so `gdb ./build/<sample>` works immediately. What you can see from the client side:

- your calls into the OO API (`IAttachment::execute`, `IStatement::openCursor`, …) and the y-valve dispatch under them (`libfbclient` exports the y-valve entry points);
- the wire, with no debugger at all: `strace -f -e trace=network ./build/<sample>` shows the `op_attach`/`op_execute`/`op_fetch` round-trips as socket traffic, and the [wire-protocol document](firebird-wire-protocol.md) decodes what is in them;
- errors, properly: every sample routes exceptions through `IUtil::formatStatus`, which renders the full status vector — the engine's error chains (e.g. the three-line `update conflict` cascade in [transactions](transactions-and-concurrency.md)) are far more informative than the top-level SQLSTATE.

## The debugger you already have: the engine's own instrumentation

For many questions in these documents a debugger is the *second*-best tool. The engine ships its own introspection, usable against the stripped production server, in production, with no restart:

| Question | Tool | Document |
|---|---|---|
| What is this attachment doing right now? | `MON$ATTACHMENTS` / `MON$STATEMENTS` / `MON$CALL_STACK` | [monitoring](monitoring-and-tuning.md) |
| What statements ran, with what plans? | Trace API / `fbtracemgr` | [trace & audit](trace-and-audit.md) |
| Where did the time go, line by line? | `RDB$PROFILER` | [profiler](profiler.md) |
| Who is blocking whom? | `fb_lock_print` (owners `-o`, locks `-l`, history `-h`) | [lock manager](lock-manager.md) |
| What is on this page / in this file? | `gstat`, and `od`/`xxd` against [ODS offsets](on-disk-structure.md) | [on-disk structure](on-disk-structure.md) |
| What BLR did my SQL become? | `isql` + `SET BLOB ALL` on the system tables | [BLR](blr-intermediate-language.md) |
| What did the engine log? | `firebird.log` in the Firebird root | — |

A useful discipline: reach for gdb when you need to see *how* a code path runs; reach for the instrumentation when you need to see *that* it ran, or how often, or how long it took.

## Per-subsystem breakpoint index

Each hands-on section lists its own breakpoints with discussion; collected here for reference, they double as a map of the engine's entry points (all paths under `src/`):

| Subsystem | Break on | Source |
|---|---|---|
| SQL → parse tree | `dsqlPass`, `Parser::parse` | `dsql/dsql.cpp`, `dsql/parse.y` |
| BLR → executable request | `PAR_parse`, `Statement::makeStatement` | `jrd/par.cpp`, `jrd/Statement.cpp` |
| Optimizer | `Optimizer::optimize` | `jrd/optimizer/Optimizer.cpp` |
| Execution | `EXE_start`, `EXE_receive`, `RecordSource::getRecord` | `jrd/exe.cpp`, `jrd/recsrc/` |
| Transactions | `TRA_start`, `TRA_commit`, `TRA_rollback` | `jrd/tra.cpp` |
| Record versions (MVCC) | `VIO_store`, `VIO_modify`, `VIO_erase`, `VIO_garbage_collect` | `jrd/vio.cpp` |
| Page cache | `CCH_fetch`, `CCH_mark`, `CCH_flush` | `jrd/cch.cpp` |
| Physical I/O | `PIO_read`, `PIO_write` | `jrd/os/posix/unix.cpp` |
| Lock manager | `LockManager::enqueue`, `blocking_action` | `lock/lock.cpp` |
| Sort | `Sort::put`, `Sort::sort` | `jrd/sort.cpp` |
| Blobs | `blb::create2`, `blb::getSegment` | `jrd/blb.cpp` |
| Events | `EventManager::postEvent` | `jrd/event.cpp` |
| Wire server side | `process_packet` | `remote/server/server.cpp` |

## Further research

- [`doc/README.build.posix.html`](https://github.com/FirebirdSQL/firebird/blob/master/doc/README.build.posix.html) — the project's own build instructions, including the CMake cross-build path.
- [`gen/Debug` vs `gen/Release`] — the two build flavours produced by `--enable-debug`; the DEV_BUILD macro gates the extra checks (`grep -r DEV_BUILD src/common/` is an instructive read).
- [GDB documentation](https://sourceware.org/gdb/current/onlinedocs/gdb/) — pending breakpoints, `info threads`, conditional breakpoints (`break VIO_modify if relation->rel_id > 128` filters out system tables).
- Companion docs: [request lifecycle](request-lifecycle-code-trace.md) (the full path a breakpoint backtrace will show) · [threading](threading-and-synchronization.md) (what `info threads` means) · [monitoring](monitoring-and-tuning.md), [trace & audit](trace-and-audit.md), [profiler](profiler.md) (the non-gdb toolbox).
