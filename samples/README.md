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
