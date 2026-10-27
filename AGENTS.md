# AGENTS.md — Working in This Repository

This repo is a paper plus a living document collection: the 2005 conceptual-architecture
paper ([README.md](README.md)) extended with **46 companion documents** that dissect
Firebird 6 subsystem by subsystem, each with runnable samples in [`samples/`](samples/).
Start with [READING-GUIDE.md](READING-GUIDE.md) for the map.

## Layout

- `*.md` (root) — the paper (`README.md`), the guide (`READING-GUIDE.md`), 46 companions.
- `samples/` — one verified sample per companion per language (see [samples/README.md](samples/README.md)):
  `cpp/`, `fb-cpp/`, `nodejs/`, `fpc/`, `rust/`, `python/`, `go/`, `java/`, `csharp/`,
  plus `sql/`, top-level `client_test.cpp`, `protocol_client.cpp`, `events_demo.cpp`.
- `tools/grammar_to_mermaid.py` + `diagrams/` — regenerable grammar diagrams.
- `extern/` — **upstream git submodules** (`firebird`, `fb-cpp`, `fbintf`, `fire-crab`,
  `firebird-qa`). Never edit in place; update via submodule bumps only.

## Standards every change must keep

- **Grounded, not paraphrased.** Claims cite the vendored `extern/firebird` source
  (file + symbol) and, wherever possible, quote output verified live against Firebird 6.
- **Every companion ends with a Hands-on section**: verified sample output, things to
  try, gdb breakpoints into the engine functions discussed.
- **Diagrams are text.** Mermaid, inline; validate with the Mermaid parser before commit.
- **Links must resolve.** Internal links are relative; check external links you touch.
- **Counts are words in prose.** The collection size appears spelled out in `README.md`
  (×2), `READING-GUIDE.md`, and `firebird-rust-conversion.md` (which counts only the
  44 docs that ship samples — it excludes itself and the debugging guide). Bump them
  together when adding a document.

## Samples conventions

- Shared helpers per language: `cpp/fb_sample.h`, `fb-cpp/fbcpp_sample.h`,
  `nodejs/common.js`, `fpc/FBHandsOn.pas`, `rust/src/lib.rs`, `python/fbsample.py`,
  `go/fbsample/`, `java/.../FbSample.java`, `csharp/FbSample.cs`. Reuse them; don't
  re-implement attach/query boilerplate.
- Defaults: demo server `inet://localhost/employee`, `SYSDBA`/`masterkey` (via
  `ISC_USER`/`ISC_PASSWORD`); scratch DBs under `/tmp/fbhandson/<topic>_<lang>.fdb`.
- Naming: `snake_case` topic files in cpp/fb-cpp/nodejs/fpc/rust/python/go;
  `PascalCase` classes in java/csharp. Avoid stdlib-shadowing names (`threading`,
  `trace`, `types` in Python; `types` in FPC) — use the suffixed form
  (`threading_demo.py`, `types_demo.pas`, …).
- Driver gaps are documented, not hidden. If a driver can't reach a feature, the twin
  declares the delta (see [samples/README.md](samples/README.md) for each family's
  known gaps) and the Hands-on section says so honestly.
- Deliberate stand-ins, not gaps: `nodejs` uses `query.js`/`srp-handshake.js` for the
  api-styles/protocol topics; top-level `protocol_client.cpp` / `events_demo.cpp`
  cover those topics for OO-API C++.
- C++ needs `-g` (gdb walk-throughs depend on it); CMake already sets it.

## Build & run

```sh
git submodule update --init extern/firebird extern/fb-cpp extern/fbintf   # as needed
cmake -B build samples && cmake --build build     # C++ + fb-cpp (needs Boost, fbclient)
make -C samples/fpc                               # Free Pascal -> fpc/bin/
cd samples/rust && cargo run --bin <topic>        # Rust
python3 samples/python/<topic>.py                 # Python (pip install -r samples/python/requirements.txt)
cd samples/go && go run ./<topic>                 # Go (pure-Go driver, no fbclient)
cd samples/java && mvn -q compile exec:exec -Dsample=<Topic>
cd samples/csharp && dotnet run -- <Topic>
cd samples/nodejs && npm install && node <topic>.js
```

Live verification needs a Firebird server with the `employee` database; create
`/tmp/fbhandson` (`chmod 777` — the server writes there as its own user).

## Adding a new companion document

1. Write `<topic>.md` in the collection's voice (grounded claims, four-engine
   comparison where it fits, Hands-on section).
2. Add one twin sample per language following the conventions above.
3. Register it: bullet in `README.md`'s companion list, track + reading paths in
   `READING-GUIDE.md`, spelled-out counts (see above).
4. Verify: build/run every twin, paste real output, validate Mermaid, check links.

## Don'ts

- Don't commit build outputs or databases (`build/`, `target/`, `bin/`, `*.fdb`,
  `node_modules/` are gitignored) — keep `git status` clean.
- Don't edit `extern/*` (they're upstream checkouts) or paste secrets/credentials.
- Don't renumber or rewrite history (`fire-crab` bump commits are the norm here).
