# Migration and Interoperability

Databases rarely live in isolation: you upgrade Firebird from one version to the next, you move data between Firebird and PostgreSQL/MySQL/SQLite, and you run heterogeneous queries that reach across engines. This document covers all three for Firebird 6 — version upgrades, engine-to-engine migration, and live interoperability — grounded in the vendored compatibility docs and demonstrated with a working flat-file interchange table on a live server, then compares the migration/interoperability story with PostgreSQL, MySQL and SQLite.

It draws on several companions: [backup and recovery](backup-and-recovery.md) (`gbak` as the migration engine), [SQL dialect and data types](sql-dialect-and-types.md) (type mapping), [client APIs and drivers](client-apis-and-drivers.md) (ETL connectivity), and [connection pooling](connection-pooling.md) (`EXECUTE STATEMENT ON EXTERNAL` for federation).

**Table of Contents**

* [Three kinds of migration](#three-kinds-of-migration)
* [Firebird version upgrades](#firebird-version-upgrades)
* [Migrating between Firebird and other engines](#migrating-between-firebird-and-other-engines)
* [Type mapping](#type-mapping)
* [Runtime interoperability](#runtime-interoperability)
* [Flat-file interchange (validated)](#flat-file-interchange-validated)
* [Comparison: PostgreSQL, MySQL, SQLite](#comparison-postgresql-mysql-sqlite)
* [Discussion](#discussion)
* [Further research](#further-research)

## Three kinds of migration

"Migration" means three different things; the tools and difficulty differ for each:

```mermaid
flowchart LR
    subgraph A["1. Version upgrade"]
        A1["Firebird N → N+1<br/>(ODS change)"]
    end
    subgraph B["2. Engine migration"]
        B1["Firebird ↔ PostgreSQL /<br/>MySQL / SQLite / InterBase"]
    end
    subgraph C["3. Runtime interoperability"]
        C1["live heterogeneous access<br/>(ODBC/JDBC, EXTERNAL, flat files)"]
    end
```

_Figure 1: Three kinds of migration — upgrading a Firebird version, moving between engines, and interoperating live_

- **Version upgrade** — same engine, newer release; the on-disk structure (ODS) may change, requiring a backup/restore.
- **Engine migration** — moving schema and data between *different* databases; needs SQL-dialect translation, type mapping, and an ETL path.
- **Runtime interoperability** — querying across engines while both run, via bridges (ODBC/JDBC), Firebird's `EXECUTE STATEMENT ON EXTERNAL`, or flat-file exchange.

## Firebird version upgrades

Firebird's on-disk structure (ODS) is versioned (see the [on-disk structure document](on-disk-structure.md#ods-versions)): ODS 12 = FB3, 13.0 = FB4, 13.1 = FB5, 14.0 = FB6. The upgrade rule follows from that:

- A **minor** ODS bump (e.g. 13.0 → 13.1) can be applied in place — a newer server opens the older database with some new features unavailable.
- A **major** ODS change (e.g. 13.x → 14.0) requires a **`gbak` backup/restore** cycle: back up on the old version, restore on the new one, which rebuilds every page in the new format (see [backup and recovery](backup-and-recovery.md#gbak-logical-backup)). This is the canonical, always-available upgrade path — and because a restore also defragments and rebuilds indexes, it doubles as maintenance.

Beyond the file format, each release documents its **behavioural incompatibilities** ([`README.incompatibilities.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/README.incompatibilities.txt), [`README.incompatibilities.3to4.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/README.incompatibilities.3to4.txt)) — SQL-syntax tightening, reserved words, API changes, and security hardening. The archetypal example is the **deprecation of UDF** in Firebird 4 (`UdfAccess = None` by default; the `ib_udf`/`fbudf` libraries removed), pushing users to the safer [UDR](extensibility.md) mechanism. Reading the incompatibilities list before a major upgrade is the standard discipline. Firebird's **InterBase heritage** also makes migration *from* legacy InterBase straightforward — the two share a lineage, and dialect 1 (see [SQL dialect](sql-dialect-and-types.md#firebird-sql-dialects)) exists precisely for that continuity.

## Migrating between Firebird and other engines

Moving between Firebird and a different engine is a schema + data + logic problem:

- **Schema and DDL** — mostly standard SQL, but each engine has proprietary syntax (identity/generators, computed columns, domains). Because Firebird targets the SQL standard in dialect 3, portable schema translates cleanly; the engine-specific parts (PSQL vs PL/pgSQL, packages, triggers — see the [PSQL document](psql-and-stored-procedures.md)) must be rewritten.
- **Data** — the bulk-transfer problem, solved with one of: a driver-based ETL script ([any language](client-apis-and-drivers.md)), an ODBC/JDBC bridge feeding a migration tool, `gbak` (Firebird↔Firebird only), or flat-file interchange (below).
- **Types** — mapped per the table in the next section.

The common tools: **JDBC** (via [Jaybird](https://github.com/FirebirdSQL/jaybird)) or **ODBC** (via the [Firebird ODBC driver](https://github.com/FirebirdSQL/firebird-odbc-driver)) let generic migration tools (ESF Database Migration Toolkit, DBeaver, Pentaho, and the like) read or write Firebird; going the other way, tools like [ora2pg](https://ora2pg.darold.net/) exemplify the engine-specific converters that exist for popular source databases. There is no single universal converter; the practical path is a bridge plus a tool, or a small script over a [driver](client-apis-and-drivers.md).

## Type mapping

The core type correspondences when migrating (see [SQL dialect and data types](sql-dialect-and-types.md) for detail; ⚠ marks a lossy or attention-needing mapping):

| Firebird | PostgreSQL | MySQL | SQLite |
|---|---|---|---|
| `SMALLINT`/`INTEGER`/`BIGINT` | same | same | `INTEGER` |
| `INT128` | `numeric` ⚠ | `DECIMAL` ⚠ | `INTEGER`/`TEXT` ⚠ |
| `NUMERIC`/`DECIMAL(p,s)` | `numeric` | `DECIMAL` | `NUMERIC` (affinity) |
| `DECFLOAT` | `numeric` ⚠ | `DECIMAL` ⚠ | `REAL`/`TEXT` ⚠ |
| `FLOAT`/`DOUBLE PRECISION` | `real`/`double precision` | `FLOAT`/`DOUBLE` | `REAL` |
| `BOOLEAN` | `boolean` | `TINYINT(1)` | `INTEGER` |
| `VARCHAR(n) CHARACTER SET …` | `varchar(n)` (+ DB encoding) ⚠ | `VARCHAR(n)` charset | `TEXT` ⚠ |
| `BLOB SUB_TYPE TEXT` | `text` | `LONGTEXT` | `TEXT` |
| `BLOB SUB_TYPE BINARY` | `bytea` | `LONGBLOB` | `BLOB` |
| `TIMESTAMP [WITH TIME ZONE]` | `timestamp[tz]` | `DATETIME`/`TIMESTAMP` ⚠ | `TEXT` ⚠ |
| `CHAR(16) CHARACTER SET OCTETS` (UUID) | `uuid` | `BINARY(16)` | `BLOB` |

The lossy cases cluster around the types Firebird has and the target lacks — `INT128`/`DECFLOAT` (only PostgreSQL/MySQL arbitrary-precision `numeric` approximates them), named-zone timestamps, and per-column character sets — mirroring the [type-system comparison](sql-dialect-and-types.md#data-type-mapping-across-the-four-systems).

## Runtime interoperability

For *live* cross-engine access without a full migration, Firebird offers:

- **ODBC / JDBC bridges** — a JDBC or ODBC connection lets ETL tools and application frameworks treat Firebird as one of several interchangeable sources/targets. This is the universal integration surface.
- **`EXECUTE STATEMENT ... ON EXTERNAL DATA SOURCE`** — run SQL on another Firebird database from PSQL, with results streamed back and connections pooled (see [connection pooling](connection-pooling.md#firebirds-external-connections-pool)). It is primarily Firebird↔Firebird.
- **External-file tables** — a table whose rows are stored in a flat OS file (`CREATE TABLE … EXTERNAL FILE '…'`), for bulk import/export interchange, demonstrated below.

Firebird deliberately does *not* have PostgreSQL's rich foreign-data-wrapper ecosystem (arbitrary remote engines as local tables); its heterogeneous story is ODBC/JDBC at the edges plus Firebird-to-Firebird `EXECUTE STATEMENT` and flat files.

## Flat-file interchange (validated)

Firebird's **external-file tables** map a SQL table onto a plain fixed-width OS file — usable simultaneously as a table and as an interchange file. Verified on a live server (with `ExternalFileAccess` enabled):

```sql
CREATE TABLE ext_people EXTERNAL FILE '/tmp/fbext/people.dat'
  (id CHAR(5), name CHAR(20), nl CHAR(1));
INSERT INTO ext_people VALUES ('1', 'Ada Lovelace', ascii_char(10));
INSERT INTO ext_people VALUES ('2', 'Grace Hopper', ascii_char(10));
SELECT count(*) FROM ext_people;   -- 2
```

The rows inserted through SQL appeared as a fixed-width flat file on disk (real output):

```text
$ cat /tmp/fbext/people.dat
1    Ada Lovelace
2    Grace Hopper
```

The same file is a SQL table to Firebird and a flat file to any other tool — a simple, dependency-free interchange for bulk load/unload. (It requires `ExternalFileAccess` to be configured, off by default for security — see [deployment](deployment-and-operations.md).)

## Comparison: PostgreSQL, MySQL, SQLite

| Aspect | **Firebird** | **PostgreSQL** | **MySQL** | **SQLite** |
|---|---|---|---|---|
| Version upgrade | `gbak` restore (major ODS) / in place (minor) | `pg_upgrade` / dump-restore | in-place / dump-restore | **None** (stable file format) |
| Logical dump | `gbak` (binary) + `isql` DDL | [`pg_dump`](https://www.postgresql.org/docs/current/app-pgdump.html) (portable SQL) | [`mysqldump`](https://dev.mysql.com/doc/refman/8.4/en/mysqldump.html) (SQL) | [`.dump`](https://sqlite.org/cli.html) (SQL) |
| Bulk load/unload | External-file tables | [`COPY`](https://www.postgresql.org/docs/current/sql-copy.html) | [`LOAD DATA`](https://dev.mysql.com/doc/refman/8.4/en/load-data.html) | [`.import`/`.dump`](https://sqlite.org/cli.html) |
| Foreign engines (live) | ODBC/JDBC; `EXECUTE … ON EXTERNAL` (FB↔FB) | **[Foreign data wrappers](https://wiki.postgresql.org/wiki/Foreign_data_wrappers)** (many engines) | FEDERATED (MySQL↔MySQL) | [`ATTACH`](https://sqlite.org/lang_attach.html) (SQLite files) |
| Bridges | ODBC / JDBC / .NET | ODBC / JDBC / libpq | ODBC / JDBC | bindings only |
| File portability | `.fdb` cross-platform (same ODS) | data dir not portable | data dir not portable | **`.sqlite` fully portable** |
| Migration in tools | via ODBC/JDBC | pg_dump / [ora2pg](https://ora2pg.darold.net/) etc. | [Workbench migration](https://dev.mysql.com/doc/workbench/en/wb-migration.html) | trivial (copy file) |
| Standards target | SQL:2016 (dialect 3) | SQL:2016 | SQL (with modes) | SQL subset |

## Discussion

**Firebird's version-migration story is `gbak`, and it is a genuine strength doing double duty.** A logical backup/restore is the always-available cross-version *and* cross-platform path, and because the restore rebuilds every page and index it simultaneously upgrades the ODS, defragments, and reclaims space — one operation where PostgreSQL might use `pg_upgrade` for speed or `pg_dump` for portability as separate choices. The trade-off is that `gbak` is Firebird-specific and binary; there is no portable-SQL dump equivalent to `pg_dump`'s text output, so *engine-to-engine* migration falls back to ODBC/JDBC + tools rather than a native dump the other engine can read.

**Live foreign-engine access is where PostgreSQL pulls decisively ahead.** PostgreSQL's foreign-data-wrapper ecosystem turns almost any remote system — other PostgreSQL, Oracle, MySQL, files, REST APIs — into local tables, a breadth no other engine here matches (it flows from the same [extensibility](extensibility.md#comparison-postgresql-mysql-sqlite) that makes PostgreSQL the extension champion). Firebird's heterogeneous story is narrower and more pragmatic: ODBC/JDBC at the boundary, `EXECUTE STATEMENT ON EXTERNAL` for Firebird-to-Firebird, and external-file tables for flat interchange. MySQL's FEDERATED engine is similarly MySQL-to-MySQL; SQLite's `ATTACH` reaches only other SQLite files. So for building a federated query layer, PostgreSQL is the clear pick; for the common cases (upgrade, dump/restore, ETL through a driver), all four are workable.

**SQLite's `.sqlite` file is the portability champion, by having almost nothing to migrate.** Because the [file format is stable and self-contained](embedded-architecture-comparison.md) across versions and architectures, "migration" for SQLite is usually copying a file — no ODS upgrade, no server, no dump. Firebird's `.fdb` is likewise cross-platform within an ODS, but a major version can require the `gbak` cycle. This is the [embedded-vs-server split](embedded-architecture-comparison.md) once more: the server engines invest in migration tooling because their formats and features evolve; SQLite optimizes for the file just working everywhere, forever.

## Further research

**Firebird**

- [`README.incompatibilities.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/README.incompatibilities.txt) and [`README.incompatibilities.3to4.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/README.incompatibilities.3to4.txt) — the per-version behavioural changes to review before upgrading.
- The [backup and recovery document](backup-and-recovery.md) (`gbak` as the migration engine), [SQL dialect and data types](sql-dialect-and-types.md) (type mapping), [client APIs and drivers](client-apis-and-drivers.md) (ETL connectivity, [Jaybird](https://github.com/FirebirdSQL/jaybird), [ODBC](https://github.com/FirebirdSQL/firebird-odbc-driver)), and [connection pooling](connection-pooling.md) (`EXECUTE STATEMENT ON EXTERNAL`).

**PostgreSQL, MySQL, SQLite**

- PostgreSQL: [`pg_dump`](https://www.postgresql.org/docs/current/app-pgdump.html), [`COPY`](https://www.postgresql.org/docs/current/sql-copy.html), [foreign data wrappers](https://wiki.postgresql.org/wiki/Foreign_data_wrappers) / [`postgres_fdw`](https://www.postgresql.org/docs/current/postgres-fdw.html), [ora2pg](https://ora2pg.darold.net/).
- MySQL: [`mysqldump`](https://dev.mysql.com/doc/refman/8.4/en/mysqldump.html), [`LOAD DATA`](https://dev.mysql.com/doc/refman/8.4/en/load-data.html), [Workbench migration](https://dev.mysql.com/doc/workbench/en/wb-migration.html).
- SQLite: [command-line shell (`.dump`/`.import`)](https://sqlite.org/cli.html), [`ATTACH DATABASE`](https://sqlite.org/lang_attach.html).

**Standards / bridges**

- [ODBC overview](https://learn.microsoft.com/en-us/sql/odbc/reference/odbc-overview) and [SQL:2016](https://en.wikipedia.org/wiki/SQL:2016) — the common ground migration leans on.
