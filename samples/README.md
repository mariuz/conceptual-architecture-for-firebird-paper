# Samples

Runnable C++ examples that exercise the architecture described in the
[paper](../README.md), using the modern object-oriented client API
(`firebird/Interface.h`) that is the supported interface in Firebird 3
and later, including the Firebird 6 development branch.

## Contents

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

- **`nodejs/`** — the same connection from Node.js, two ways:
  `query.js` uses the pure-JavaScript **node-firebird** driver, and
  `srp-handshake.js` re-implements the wire protocol and the Srp256/Arc4
  handshake from scratch with no dependencies. See
  [../firebird-wire-protocol.md](../firebird-wire-protocol.md).

## Prerequisites

- A C++17 compiler.
- The Firebird API headers, provided by the `extern/firebird` submodule:

  ```sh
  git submodule update --init extern/firebird
  ```

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
