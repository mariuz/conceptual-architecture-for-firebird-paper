# PSQL, Stored Procedures and Triggers

A database's **procedural language** decides how much application logic can live inside the engine — stored procedures, functions, triggers, exception handling, cursors. Firebird's is called **PSQL** (Procedural SQL). This document describes PSQL and its module types, grounded in the vendored `doc/sql.extensions/` reference set and demonstrated with working code on a live Firebird 6 server, then compares it — with side-by-side examples for the same tasks — against PostgreSQL's PL/pgSQL, MySQL's stored routines, and SQLite (which has no stored-procedure language at all).

It is a companion to the [main paper](README.md) and pairs with the [SQL dialect and data types document](sql-dialect-and-types.md) (the types PSQL variables use) and the [grammar document](grammar-and-parser.md) (PSQL is part of the same grammar). External (non-PSQL) routines — UDR and plugins — are a distinct topic touched on only briefly here.

**Table of Contents**

* [What PSQL is](#what-psql-is)
* [The PSQL module types](#the-psql-module-types)
* [Selectable procedures: SUSPEND](#selectable-procedures-suspend)
* [Triggers: DML, DDL and database](#triggers-dml-ddl-and-database)
* [What firing a trigger actually costs the writer](#what-firing-a-trigger-actually-costs-the-writer)
* [A generator draw is not a value, it is a write](#a-generator-draw-is-not-a-value-it-is-a-write)
* [Exception handling and other features](#exception-handling-and-other-features)
* [Worked examples (validated on Firebird 6)](#worked-examples-validated-on-firebird-6)
* [Side-by-side: the same procedure in four systems](#side-by-side-the-same-procedure-in-four-systems)
* [Side-by-side: the same trigger](#side-by-side-the-same-trigger)
* [Comparison table](#comparison-table)
* [Discussion](#discussion)
* [Further research](#further-research)

## What PSQL is

PSQL is Firebird's built-in procedural extension to SQL: a block-structured language with variables, control flow (`IF`, `WHILE`, `FOR`), cursors, exception handling and transaction-aware statements, compiled — like every request — to **BLR** and executed by the engine (see the [architecture comparison](architecture-comparison.md#firebird-recap) and [grammar document](grammar-and-parser.md)). It is a *single, dedicated* in-engine language: unlike PostgreSQL, Firebird does not host multiple procedural languages in the server; logic that PSQL cannot express is written as an external **UDR** routine instead.

Every PSQL body shares the same shape: an optional variable-declaration section, then a `BEGIN ... END` block, with `:variable` referencing PSQL variables inside SQL statements.

## The PSQL module types

```mermaid
flowchart TB
    PSQL["PSQL — compiled to BLR, run in-engine"]
    PSQL --> EP["executable procedure<br/>EXECUTE PROCEDURE"]
    PSQL --> SP["selectable procedure<br/>SELECT ... FROM proc (SUSPEND)"]
    PSQL --> FN["stored function<br/>(scalar, FB3+)"]
    PSQL --> TR["triggers<br/>DML · DDL · database-event"]
    PSQL --> PK["packages<br/>header + body (FB3+)"]
    PSQL --> EB["EXECUTE BLOCK<br/>anonymous inline PSQL"]
    PSQL --> SR["sub-procedures / sub-functions<br/>(local to a module)"]
```

_Figure 1: PSQL module types — procedures (executable and selectable), functions, triggers, packages, anonymous blocks and local subroutines_

- **Executable procedure** — called with `EXECUTE PROCEDURE`; may take inputs and return a single set of output parameters.
- **Selectable procedure** — queried with `SELECT ... FROM proc(args)`; streams a result set via `SUSPEND` (see below).
- **Stored function** (FB3+) — returns a scalar with `RETURN`; usable in expressions.
- **Triggers** — fire on DML, DDL or database events.
- **Packages** (FB3+) — a header declaring procedures/functions and a body implementing them, grouping related routines with a public/private boundary (an Oracle-style feature).
- **`EXECUTE BLOCK`** — an anonymous PSQL block run ad hoc, as if it were a procedure body inline in a query.
- **Sub-procedures / sub-functions** — routines declared locally inside another PSQL module.

## Selectable procedures: SUSPEND

Firebird's most distinctive PSQL feature is the **selectable procedure**: a procedure that produces a *result set* you `SELECT` from, one row at a time, using `SUSPEND` to yield each row. The engine drives it like a table:

```sql
CREATE PROCEDURE raises (pct NUMERIC(5,2))
RETURNS (id INTEGER, name VARCHAR(30), new_salary NUMERIC(10,2))
AS
BEGIN
  FOR SELECT id, name, salary FROM emp INTO :id, :name, :new_salary DO
  BEGIN
    new_salary = new_salary * (1 + pct/100);
    SUSPEND;              -- emit this row to the caller
  END
END
```

`SELECT * FROM raises(10)` then returns a computed row per employee. This turns a procedure into a parameterized, composable view — you can join it, filter it, order it. PostgreSQL expresses the same idea with set-returning functions (`RETURNS SETOF ... RETURN NEXT/RETURN QUERY`); MySQL cannot (a MySQL procedure emits a result set only as a side effect of a bare `SELECT`, not composably); SQLite has no equivalent.

## Triggers: DML, DDL and database

Firebird triggers are unusually broad. Beyond ordinary row triggers, it fires on schema changes and on connection/transaction lifecycle events:

```mermaid
flowchart TB
    subgraph DML["DML triggers (per row)"]
        D1["BEFORE / AFTER"]
        D2["INSERT / UPDATE / DELETE"]
        D3["universal: INSERT OR UPDATE OR DELETE<br/>with INSERTING/UPDATING/DELETING"]
        D4["POSITION n (fire order)"]
    end
    subgraph DDL["DDL triggers"]
        E1["BEFORE/AFTER CREATE/ALTER/DROP <object>"]
    end
    subgraph DBT["Database (event) triggers"]
        F1["ON CONNECT / DISCONNECT"]
        F2["ON TRANSACTION START / COMMIT / ROLLBACK"]
    end
```

_Figure 2: Firebird trigger kinds — row-level DML (with universal multi-action and POSITION), DDL triggers, and database-event triggers_

- **DML triggers** — `BEFORE`/`AFTER` `INSERT`/`UPDATE`/`DELETE`, with the `NEW.` and `OLD.` context records. **Universal triggers** (`INSERT OR UPDATE OR DELETE`) handle several actions in one body using the `INSERTING`/`UPDATING`/`DELETING` booleans, and **`POSITION n`** orders multiple triggers on the same event.
- **DDL triggers** ([`README.ddl_triggers.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.ddl_triggers.txt)) — fire on `CREATE`/`ALTER`/`DROP` of objects, for schema-change auditing or policy enforcement. A `BEFORE` body that raises **refuses the statement**, which is what makes them a policy rather than a log; the body asks which statement fired it through a namespace of its own, `RDB$GET_CONTEXT('DDL_TRIGGER', 'DDL_EVENT' | 'OBJECT_NAME' | 'SQL_TEXT')`. The failure the client sees is the engine's standard DDL wrapper — `unsuccessful metadata update`, then `DROP VIEW "PUBLIC"."V" failed`, then the trigger's own exception — not the exception alone.

  All three classes share one column, `RDB$TRIGGER_TYPE`, and its top bits say which class a row belongs to: `>> 13 & 3` is 0 for a relation's DML trigger, 1 for a database trigger, 2 for a DDL trigger. A DDL trigger's remaining bits are a *set* of events, so one trigger can fire for many, and `ANY DDL STATEMENT` is simply all of them at once.
- **Database triggers** ([`README.db_triggers.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.db_triggers.txt)) — fire `ON CONNECT`, `ON DISCONNECT`, and on transaction `START`/`COMMIT`/`ROLLBACK` — e.g. to set up session context or log connections. PostgreSQL has DDL/event triggers but not connection triggers in core; MySQL and SQLite have neither.

  Three of their rules are worth knowing before you rely on one.
  **`ON CONNECT` is a gate, not a notification**: if its body raises, the
  attach is refused and the client gets that exception, which is how a
  database locks itself against logins. It runs *before* the client's
  first transaction, in a transaction of its own — so its work is
  committed independently of anything the session goes on to do, and it
  fires no `ON TRANSACTION START` of its own. And **the transaction
  triggers fire inside the transaction they are firing for**, which has a
  sharp consequence for the rollback one: everything an `ON TRANSACTION
  ROLLBACK` body writes is rolled back with it, so such a body must use
  `IN AUTONOMOUS TRANSACTION` to leave any trace. (A generator it drew
  from has still moved: a draw is not transactional.)

## What firing a trigger actually costs the writer

The trigger *list* above is the easy part. What a DML implementation has
to get right is the **order things happen in around one row**, and it is
tighter than it looks:

1. the statement's own values are placed, then the column **DEFAULTs**
   for anything omitted;
2. **BEFORE** triggers run, in `RDB$TRIGGER_SEQUENCE` order, each seeing
   what the previous one left. A `BEFORE` body may assign `NEW.<col>`,
   and that assignment *is* the stored value — it overwrites what the
   client sent;
3. the **CHECK constraints** and then the per-field validations
   (`NOT NULL`, domain checks) run over the row as the triggers left it —
   which is why a `BEFORE` trigger can satisfy a `NOT NULL` column the
   client never mentioned;
4. the row is **stored** and its index entries written;
5. **AFTER** triggers run, with the row in place. `NEW` is read-only
   here; what an AFTER body can still do is act — write another table —
   or raise.

A raise anywhere in that sequence takes the whole statement back, and it
carries a stack item naming the trigger and the position *in the original
`CREATE TRIGGER` text*: `At trigger "PUBLIC"."T_BI3" line: 1, col: 82`.
Those numbers do not come from the stored source (which starts after
`AS`) — they come from the trigger's **`RDB$DEBUG_INFO`** blob, which maps
each statement to a line and column in the text as written.

Two consequences worth stating for anyone reimplementing this:

- **A trigger changes what a write means.** A server that stores the row
  without firing the triggers has not "skipped an extension" — it has
  written different data, and no error anywhere says so. Refusing the
  statement is the only honest alternative to firing it.
- **A trigger body is a statement of its own.** A body that writes
  another table needs the same machinery a client's `INSERT` does, in the
  middle of an already-running write. What makes that hard is not the
  writing but the *reading*: the moment a body asks the database a
  question, the answer pins exactly when it ran.

  The engine's answer is per-row, and it is observable. Under an
  `INSERT ... SELECT` of three rows, a `BEFORE INSERT` body running
  `SELECT COUNT(*)` on its own table answers **0, 1, 2** — it sees every
  earlier row of the same statement and not the one being written. An
  `AFTER` body counts itself in. A `BEFORE UPDATE` body reads the sum
  *before* this row's update and a `BEFORE DELETE` body still counts the
  row it is about to remove. So a body can be run neither before the
  statement nor after it: batching the rows and firing the bodies around
  the batch answers 3, 3, 3, and is wrong in a way no error reports.

  In [fire-crab](firebird-rust-conversion.md) this is the reason a
  statement hands its working copy of the file *back* around every such
  body and takes a fresh one after. Publishing is what puts the body's
  read on the file the engine would show it, and it forces two things
  that are easy to miss: the statement's rows must already carry an
  adopted transaction id (an un-adopted id is another transaction's
  uncommitted work to every reader, the body included), and the rows must
  be written as they are read rather than in a final pass.

- **A loop takes its rows when it starts.** A `FOR SELECT` whose own body
  writes the table it is iterating still walks only the rows that were
  there when it opened — on a two-row table inserting one row per
  iteration, the loop runs twice and leaves four rows. A declared cursor
  behaves the same between `OPEN` and `CLOSE`. This is the difference
  between a loop that terminates and one that does not, and it is a
  property of the *statement*, not of the transaction: the body's own
  inserts are perfectly visible to the next statement.

- **A statement that fails takes the body's writes with it**, however far
  the body got — a trigger that logged a row whose own `INSERT` then
  violates a `CHECK` leaves the log empty. That is one undo window around
  the whole statement, and it is why a body's write cannot simply be
  applied and forgotten.

## A generator draw is not a value, it is a write

The commonest trigger body in Firebird is four words long:

```sql
IF (NEW.ID IS NULL) THEN NEW.ID = GEN_ID(G, 1);
```

and it hides two properties that make it much less ordinary than it
looks. The first: **generators are not transactional.** A draw is a page
write that outlives the transaction that made it, so a row rejected
after its trigger fired still consumed a value — `INSERT` a row that
violates a constraint and the sequence has moved anyway. The second: the
draw happens *inside* a statement that is already holding its working
copy of the very page the generator lives on. A trigger interpreter that
tries to perform the draw itself writes into a copy the outer statement
is about to overwrite.

[fire-crab](firebird-rust-conversion.md) settles both by running such a
body **twice**. The first pass answers 0 for every draw, over a copy of
the row, and records what the body *would* draw; the statement — which
holds the page — performs exactly those draws; the second pass replays
the values in order. Two passes are sound because a trigger body that is
otherwise pure starts from the same row both times, and the design falls
out of the two cases that decide it:

- a **conditional** draw must consume nothing when its branch is
  skipped, which rules out simply pre-drawing a value per draw site at
  prepare time;
- a body that **raises after drawing** must still consume the value,
  which the recorded-then-performed order gives for free.

What it cannot do is let a *drawn value* decide a branch — the first
pass sees 0 there and could take a different path than the second — so
that shape is refused. The distinction is finer than it sounds and worth
stating precisely, because the classic trigger above is *not* an
instance of it: it reads `NEW.ID` before anything has assigned to it.
`NEW.ID = GEN_ID(G,1); IF (NEW.ID > 100) THEN ...` is.

One more detail for anyone reading the BLR: `NEXT VALUE FOR seq` is not
sugar for `GEN_ID(seq, 1)`. They are different verbs — `blr_gen_id2`
carries the counted name alone, `blr_gen_id` carries a name and a step
expression — and the sequence form advances by the sequence's own
`INCREMENT BY`, which the two-argument form overrides.

## A body's nested statement names columns in two languages

A trigger body reaching another table is the workhorse of every schema
that keeps an audit trail:

```sql
CREATE TRIGGER LOG_IT FOR ORDERS AFTER UPDATE AS
BEGIN
  DELETE FROM ORDER_LOG WHERE ID = OLD.ID;
  UPDATE ORDER_LOG SET SEEN = SEEN + 1 WHERE ID = OLD.ID;
END
```

Read that `ID` on the left of each `WHERE` and the `OLD.ID` on the
right. They are spelled with the same letters and they come from
different worlds: the bare name is a column of `ORDER_LOG`, resolved per
row as the nested statement scans it, while `OLD.ID` is a single value
belonging to the row that fired the trigger. `SEEN + 1` is likewise
`ORDER_LOG`'s own `SEEN`, one value per row, not the fired row's. In the
BLR the distinction is explicit: every field reference carries a
*context number*, and the engine's `blr_store`/`blr_modify` nodes open a
new context for the statement's target while the trigger's `OLD` and
`NEW` keep contexts 0 and 1.

That numbering is the whole safety of the construct, and it is easy to
lose in a re-implementation that runs a body by rendering each nested
statement back to SQL text — a reasonable design, because the rendered
statement then goes down the ordinary planner and picks up index
maintenance, defaults, `NOT NULL`, `CHECK` and foreign keys for free
instead of through a second, divergent write path. Rendering means
folding what is known to a literal, and *what is known* is exactly the
question. [fire-crab](firebird-rust-conversion.md) answered a field
reference by checking whether its context was 1 — `NEW` — and treating
everything else as `OLD`. A bare column's context is neither, so the
plain name was answered with the fired row's value and baked in as a
constant:

```
DELETE FROM LG WHERE ID = OLD.ID          →  DELETE FROM LG WHERE 2 = 2
UPDATE LG SET V = V + 1 WHERE ID = OLD.ID →  UPDATE LG SET V = 21 WHERE 2 = 2
DELETE FROM LG WHERE V > 1000             →  DELETE FROM LG WHERE 20 > 1000
```

The first emptied the log table. The second wrote the *fired* row's
value over every row of it. The third never consulted `LG` at all. No
error, no exotic column type — plain `INTEGER` reaches all three — and
the one shape that stayed correct was `INSERT INTO LG (ID, V) VALUES
(OLD.ID, NEW.V)`, where every value genuinely *is* a row reference. That
is also the shape the test suite covered, which is how it survived: a
log table with a single row cannot tell a statement that hits the right
row from one that hits all of them.

The repair is to say what the contexts mean rather than what they are
not — 0 is `OLD`, 1 is `NEW`, anything else is not this row's business —
so an unqualified name refuses to fold and travels into the rendered
text as itself, for the target table's own planner to resolve.

There is a sting in the tail of that repair, and it is worth more than
the repair itself. Naming the contexts broke a *second* piece of code
that had been encoding the same law differently. Not every nested
statement is rebuilt from a parsed expression tree — values the body
grammar cannot hold (a `CASE`, a concatenation, a function call) are
kept as written, and the row references are substituted into that
*text*; a body query's `WHERE` is handled the same way. That
substitution used its own numbering: 1 for `NEW` and **2** for `OLD`,
which worked only for as long as the reader treated everything that was
not 1 as `OLD`. The moment the reader started naming its contexts, every
`OLD.` reference travelling through the text path became a refusal — and
a full 361-gate differential sweep stayed green, because no test covered
that path. One law, two encodings, one of them updated: the same shape
as the bug being fixed, one level up.

The rendering design has a second demand, less obvious: **a value that
cannot be spelled as a literal cannot be folded.** `psql_literal` had
forms for integers, exact numerics, strings, booleans and the temporal
types, and none for `DOUBLE PRECISION`, `FLOAT`, 128-bit numerics or
`DECFLOAT`. When the fold failed, the same code path emitted the field's
bare *name* — so `UPDATE LG SET AMT = NEW.AMT` became `SET AMT = AMT`,
which stores nothing and reports success. Two different bugs with one
cause: a fallthrough that silently changes what a name means.

Spelling an approximate numeric is its own small trap. The display
renderer prints a `DOUBLE` to sixteen significant digits, the way `%g`
does, and a binary64 needs seventeen in the worst case — a literal built
from the display form stores a value one unit in the last place from the
one it came from, silently and forever. What is needed is the *shortest
text that parses back to identical bits*, in exponent form so that SQL
reads it as approximate rather than as an exact numeric with a scale.
The check is arithmetic, not visual: store `1.0000000000000002`, read it
back, and compute `(x - 1) * 1e16`. Both Firebird and the conversion
answer `2.220446049250313`. The display form would have answered `0`.

## Exception handling and other features

- **Custom exceptions** — `CREATE EXCEPTION name 'message'`, raised with `EXCEPTION name` (optionally with a runtime message), and caught with `WHEN <condition> DO` blocks that can match a named exception, a SQLCODE/GDSCODE/SQLSTATE, or `ANY`. `WHEN ... DO` can retry, log, or re-raise.
- **Cursors** — explicit `DECLARE ... CURSOR`, `FOR SELECT` loops, scrollable cursors, and cursor variables.
- **Autonomous transactions** — `IN AUTONOMOUS TRANSACTION DO ...` runs a nested unit that commits independently (audit rows that survive an outer rollback) — see the [transactions document](transactions-and-concurrency.md#savepoints-explicit-locks-and-autonomous-transactions).
- **`RETURNING`** — DML inside PSQL (and at the SQL level) can return generated values (`INSERT ... RETURNING id INTO :var`).
- **Stack traces** — uncaught exceptions carry a PSQL stack trace (`At procedure "…" line: N, col: M`), shown in the exception demo below.

## Worked examples (validated on Firebird 6)

The following were created and run on a live server. An **executable procedure** with a custom exception and `RETURNING`:

```sql
CREATE EXCEPTION low_salary 'salary below minimum';

CREATE PROCEDURE hire (p_name VARCHAR(30), p_salary NUMERIC(10,2))
RETURNS (new_id INTEGER)
AS
BEGIN
  IF (p_salary < 1000) THEN EXCEPTION low_salary;
  INSERT INTO emp (name, salary) VALUES (:p_name, :p_salary) RETURNING id INTO :new_id;
END
```

A **BEFORE INSERT trigger** for audit, and a **package** with a function:

```sql
CREATE TRIGGER emp_bi BEFORE INSERT ON emp AS
BEGIN
  INSERT INTO audit_log VALUES (CURRENT_TIMESTAMP, 'insert emp ' || COALESCE(NEW.name,'?'));
END;

CREATE PACKAGE hr AS BEGIN
  FUNCTION fulltime_bonus(sal NUMERIC(10,2)) RETURNS NUMERIC(10,2);
END;
CREATE PACKAGE BODY hr AS BEGIN
  FUNCTION fulltime_bonus(sal NUMERIC(10,2)) RETURNS NUMERIC(10,2) AS
  BEGIN
    RETURN sal * 0.10;
  END
END;
```

Exercising them produced (real output):

```text
EXECUTE PROCEDURE hire('Ada', 5000);      -- NEW_ID = 1
EXECUTE PROCEDURE hire('Grace', 6000);    -- NEW_ID = 2

SELECT * FROM raises(10);                  -- Ada 5500.00 / Grace 6600.00  (selectable proc)
SELECT name, hr.fulltime_bonus(salary) FROM emp;  -- Ada 500.00 / Grace 600.00  (package fn)
SELECT count(*) FROM audit_log;           -- 2   (trigger fired twice)

EXECUTE PROCEDURE hire('Poorpay', 500);   -- raises the exception:
--   SQLSTATE = HY000, exception 1, "PUBLIC"."LOW_SALARY", salary below minimum
--   At procedure "PUBLIC"."HIRE" line: 6, col: 29
```

Every feature worked as written: the selectable procedure streamed computed rows, the trigger audited each insert, the package function evaluated in a `SELECT`, and the exception propagated with a schema-qualified (FB6 `PUBLIC`) stack trace.

## Side-by-side: the same procedure in four systems

The `hire` procedure — insert an employee, reject a too-low salary, return the new id — in each system's language:

**Firebird (PSQL):**

```sql
CREATE PROCEDURE hire (p_name VARCHAR(30), p_salary NUMERIC(10,2))
RETURNS (new_id INTEGER) AS
BEGIN
  IF (p_salary < 1000) THEN EXCEPTION low_salary;
  INSERT INTO emp (name, salary) VALUES (:p_name, :p_salary) RETURNING id INTO :new_id;
END
```

**PostgreSQL (PL/pgSQL):**

```sql
CREATE FUNCTION hire(p_name text, p_salary numeric) RETURNS integer AS $$
DECLARE new_id integer;
BEGIN
  IF p_salary < 1000 THEN RAISE EXCEPTION 'salary below minimum'; END IF;
  INSERT INTO emp (name, salary) VALUES (p_name, p_salary) RETURNING id INTO new_id;
  RETURN new_id;
END;
$$ LANGUAGE plpgsql;
```

**MySQL (SQL/PSM):**

```sql
DELIMITER //
CREATE PROCEDURE hire(IN p_name VARCHAR(30), IN p_salary DECIMAL(10,2), OUT new_id INT)
BEGIN
  IF p_salary < 1000 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'salary below minimum';
  END IF;
  INSERT INTO emp (name, salary) VALUES (p_name, p_salary);
  SET new_id = LAST_INSERT_ID();
END //
DELIMITER ;
```

**SQLite:** *not possible* — SQLite has no stored-procedure language. The logic must live in the application, or be approximated with a `BEFORE INSERT` trigger that `RAISE(ABORT, ...)`s on a low salary (validation only, no return value).

The shapes rhyme (declare, guard-with-exception, insert-returning) but differ in detail: Firebird returns via `RETURNS (...)` output parameters, PL/pgSQL via a function `RETURN`, MySQL via an `OUT` parameter + `LAST_INSERT_ID()`. Firebird and PL/pgSQL both have `INSERT ... RETURNING`; MySQL does not and uses `LAST_INSERT_ID()`.

## Side-by-side: the same trigger

An audit trigger that logs each insert:

**Firebird:**

```sql
CREATE TRIGGER emp_bi BEFORE INSERT ON emp AS
BEGIN
  INSERT INTO audit_log VALUES (CURRENT_TIMESTAMP, 'insert ' || NEW.name);
END;
```

**PostgreSQL** (a trigger *function* plus a `CREATE TRIGGER` binding — PostgreSQL's two-step model):

```sql
CREATE FUNCTION emp_audit() RETURNS trigger AS $$
BEGIN
  INSERT INTO audit_log VALUES (now(), 'insert ' || NEW.name);
  RETURN NEW;
END; $$ LANGUAGE plpgsql;
CREATE TRIGGER emp_bi BEFORE INSERT ON emp FOR EACH ROW EXECUTE FUNCTION emp_audit();
```

**MySQL:**

```sql
CREATE TRIGGER emp_bi BEFORE INSERT ON emp FOR EACH ROW
  INSERT INTO audit_log VALUES (NOW(), CONCAT('insert ', NEW.name));
```

**SQLite:**

```sql
CREATE TRIGGER emp_bi AFTER INSERT ON emp
BEGIN
  INSERT INTO audit_log VALUES (datetime('now'), 'insert ' || NEW.name);
END;
```

All four support row triggers with `NEW`/`OLD`, but note PostgreSQL's distinctive **two-object model** (a reusable trigger *function* bound by `CREATE TRIGGER`), where Firebird, MySQL and SQLite put the body directly in the trigger. SQLite's trigger body is limited to SQL statements (no variables or control flow); Firebird's and PostgreSQL's are full procedural bodies.

## Comparison table

| Feature | **Firebird (PSQL)** | **PostgreSQL** | **MySQL** | **SQLite** |
|---|---|---|---|---|
| Procedural language | PSQL (built-in) | PL/pgSQL (+ PL/Python, PL/Perl, …) | SQL/PSM | **None** |
| Stored procedures | Yes (executable) | Yes ([`CREATE PROCEDURE`](https://www.postgresql.org/docs/current/sql-createprocedure.html), v11+) | Yes | No |
| Stored functions | Yes (FB3+) | Yes (rich) | Yes | App-defined C functions only |
| Result-set procedure | **Selectable proc (`SUSPEND`)** | SETOF functions / `RETURN QUERY` | Result set via bare `SELECT` | No |
| Packages | **Yes** (header + body) | No (schemas/extensions instead) | No | No |
| Anonymous block | `EXECUTE BLOCK` | `DO` | No | No |
| DML triggers | BEFORE/AFTER, universal, POSITION | BEFORE/AFTER/INSTEAD OF, row/statement | BEFORE/AFTER row | BEFORE/AFTER/INSTEAD OF row |
| DDL triggers | **Yes** | Event triggers | No | No |
| Connection/tx triggers | **Yes** (database triggers) | No (core) | No | No |
| Trigger model | Body in trigger | **Function + binding** | Body in trigger | Body (SQL only) in trigger |
| Exception handling | `EXCEPTION` + `WHEN...DO` | `RAISE` + `EXCEPTION` block | `SIGNAL` + `DECLARE HANDLER` | `RAISE()` in triggers only |
| Autonomous tx | Yes | Via extension/dblink | No | No |
| Multiple languages | No (UDR for external) | **Yes** (pluggable PLs) | No | No (C extensions) |

## Discussion

**Firebird is one of the most capable in-engine procedural platforms — with two standout features.** Its **selectable procedures** turn a procedure into a composable, parameterized result set (join it, filter it) in a way only PostgreSQL's set-returning functions match and MySQL and SQLite cannot; and its **packages** bring Oracle-style grouping with a public/private boundary that neither PostgreSQL nor MySQL offers natively. Add DDL and database (connection/transaction) triggers, and Firebird lets you push more kinds of logic into the engine than any of the three comparators except in PostgreSQL's one dimension below.

**PostgreSQL's differentiator is pluralism, not any single feature.** It hosts *many* procedural languages (PL/pgSQL, PL/Python, PL/Perl, PL/v8, …) and uses a two-object trigger model (reusable trigger functions), which is more flexible for sharing logic across triggers. Where Firebird gives you one deep built-in language plus external UDR, PostgreSQL gives you a marketplace of in-engine languages. Both are valid answers to "how much logic belongs in the database" — Firebird bets on a single strong language, PostgreSQL on extensibility (a theme running through the whole [architecture comparison](architecture-comparison.md#discussion-what-the-contrasts-illuminate)).

**MySQL is competent but conservative, and SQLite opts out entirely.** MySQL's SQL/PSM routines cover the basics (procedures, functions, `SIGNAL`/`HANDLER` error handling, row triggers) but lack packages, selectable procedures, `RETURNING`, and DDL/connection triggers, and historically restricted triggers more than the others. SQLite has *no* procedural language at all — only SQL-only triggers — which is entirely consistent with its embedded, application-owns-the-logic design (see the [embedded comparison](embedded-architecture-comparison.md)): the application *is* the procedural layer. The split is the recurring one — server databases invest in in-engine logic; the embedded library deliberately does not.

## Hands-on: samples, tests and debugging

### C++ sample — [`samples/cpp/psql.cpp`](samples/cpp/psql.cpp)

The sample rebuilds the [worked examples](#worked-examples-validated-on-firebird-6) on a scratch database and drives one of each module type from the client. The API mechanics mirror the language distinctions: the **executable procedure** `hire` returns its output parameters as a *single message* read straight from `IStatement::execute` — there is no cursor to open, which is exactly the executable-vs-[selectable](#selectable-procedures-suspend) divide; the **selectable procedure** `raises` is fetched through `openCursor` like a table, each row a `SUSPEND`; the **BEFORE INSERT trigger** is observed only by its effect (two audit rows for two hires); and the **custom exception** arrives as an `FbException` whose status vector, rendered with `IUtil::formatStatus`, carries the schema-qualified exception name and the PSQL stack trace with line and column. Idempotency needed a PSQL-specific trick worth reading in the source: the procedures are first reduced to body-less stubs with `CREATE OR ALTER` so that `RECREATE TABLE emp` is not blocked by their stored dependencies.

```sh
cmake -B build samples && cmake --build build
./build/psql                     # default: inet://localhost//tmp/fbhandson/psql.fdb
```

Verified output:

```text
EXECUTE PROCEDURE hire('Ada', 5000)        -> NEW_ID = 1
EXECUTE PROCEDURE hire('Grace', 6000)      -> NEW_ID = 2
audit_log rows (trigger emp_bi):              2

SELECT * FROM raises(10):
ID NAME  NEW_SALARY 
-- ----- ---------- 
1  Ada   5500.00    
2  Grace 6600.00    

EXECUTE PROCEDURE hire('Poorpay', 500) ->
exception 1
-"PUBLIC"."LOW_SALARY"
-salary below minimum
-At procedure "PUBLIC"."HIRE" line: 4, col: 29
done.
```

### fb-cpp sample — [`samples/fb-cpp/psql.cpp`](samples/fb-cpp/psql.cpp)

The same four module types through [fb-cpp](https://github.com/asfernandes/fb-cpp) (vendored at [`extern/fb-cpp`](extern/fb-cpp)), the modern C++20 wrapper over the OO API. The executable-vs-selectable divide the OO-API sample expresses as `execute` versus `openCursor` surfaces here as a queryable property: `Statement::getType()` returns `StatementType::EXEC_PROCEDURE` for `EXECUTE PROCEDURE hire(...)` (one output message — `execute()` fills it, `getInt32(0)` reads it, no cursor anywhere) and `StatementType::SELECT` for the `SUSPEND`-streaming `raises(10)`, where `execute()` opens the cursor and fetches the first row and `fetchNext()` walks the rest. Output values come back as `std::optional`, `NUMERIC(10,2)` renders its scale through `getString()`, and the failing hire throws a typed `DatabaseException` whose `getErrorCode()` and `what()` carry the status vector the OO-API version formats by hand with `IUtil::formatStatus`.

```sh
cmake -B build samples && cmake --build build   # needs libboost-dev + libboost-filesystem-dev
./build/fbcpp_psql
```

Verified: `NEW_ID = 1` and `2` both tagged `[type=EXEC_PROCEDURE]`, 2 audit rows from the trigger, `raises(10)` streaming `5500.00` / `6600.00`, and the exception surfacing as `gds 335544517 = isc_except` with the identical four-line chain — `"PUBLIC"."LOW_SALARY"`, `salary below minimum`, `At procedure "PUBLIC"."HIRE" line: 4, col: 29`.

### JavaScript sample — [`samples/nodejs/psql.js`](samples/nodejs/psql.js)

The same objects called through node-firebird (`cd samples/nodejs && node psql.js`). The driver surfaces the same distinctions in JavaScript shapes: `EXECUTE PROCEDURE hire(?, ?)` resolves to a **plain object** (`{ NEW_ID: 1 }` — one output message, no cursor) while `SELECT ... FROM raises(10)` resolves to an **array of rows**; and the failing call rejects with an `Error` whose message is the identical status vector, stack trace included: `Exception 1, "PUBLIC"."LOW_SALARY", salary below minimum, At procedure "PUBLIC"."HIRE" line: 4, col: 29`.

### Rust sample — [`samples/rust/src/bin/psql.rs`](samples/rust/src/bin/psql.rs)

The same four module types through [rsfbclient](https://github.com/fernandobatels/rsfbclient), Rust's Firebird client (`cd samples/rust && cargo run --bin psql`). The executable-vs-selectable divide gets its cleanest expression yet in the type system: `tr.execute_returnable(call, ())` maps the single output message of `EXECUTE PROCEDURE hire(...)` straight into a typed tuple `(i64,)` — no cursor, no plain-object convention — while the `SUSPEND`-streaming `raises(10)` is just `tr.query(...)` collecting a `Vec<(i64, String, f64)>` like any table. The `f64` in that tuple is rsfbclient's coarse type surface showing: `NUMERIC(10,2)` arrives as a float, where the OO-API and fb-cpp samples keep the scale exact through string rendering. The custom exception comes back as an `FbError` whose `Display` is the whole status vector, stack trace included.

Verified: `NEW_ID = 1` and `2` from the two hires, 2 audit rows from the trigger, `raises(10)` streaming `5500.00` / `6600.00`, and the failing hire printing `sql error -836: exception 1` followed by the identical chain — `"PUBLIC"."LOW_SALARY"`, `salary below minimum`, `At procedure "PUBLIC"."HIRE" line: 4, col: 29` — the one delta being that rsfbclient leads with the SQLCODE where the C++ samples lead with the `exception 1` line.

### Free Pascal sample — [`samples/fpc/psql.pas`](samples/fpc/psql.pas)

The same four module types through [fbintf](https://github.com/MWASoftware/fbintf) (vendored at [`extern/fbintf`](extern/fbintf)), MWA Software's Firebird Pascal API — the layer under IBX — driving the same libfbclient as the C++ samples behind COM-style reference-counted interfaces (`make -C samples/fpc bin/psql && samples/fpc/bin/psql`). The executable-vs-selectable divide maps onto two interface types: `A.Prepare` + `IStatement.Execute` returns the one output message of `EXECUTE PROCEDURE hire(...)` as an `IResults` read by name (`Res.ByName('NEW_ID').AsInteger` — no cursor), while the `SUSPEND`-streaming `raises(10)` is `OpenCursor` + `IResultSet.FetchNext` like any table. Where rsfbclient's coarse type surface degrades `NUMERIC(10,2)` to an `f64`, fbintf reaches the value exactly: `AsCurrency` maps the scaled integer onto Pascal's fixed-point `Currency` type. The custom exception arrives as `EIBInterBaseError` whose `IBErrorCode` is the gds code and whose `Message` carries the full status-vector chain, PSQL stack trace included.

Verified: `NEW_ID = 1` and `2` from the two hires, 2 audit rows from the trigger, `raises(10)` streaming `5500.00` / `6600.00`, and the failing hire raising `gds 335544517` with the identical chain — `exception 1`, `"PUBLIC"."LOW_SALARY"`, `salary below minimum`, `At procedure "PUBLIC"."HIRE" line: 4, col: 29` — fbintf's one rendering delta being an `Engine Code: 335544517` line prefixed above the chain.

### Things to try

- Add a nested call (`hire` invoked from an `EXECUTE BLOCK`, or from a second procedure) and watch the stack trace grow to multiple `At procedure ... At block` lines — the `dbginfo` machinery described in the [BLR document](blr-intermediate-language.md#both-directions-of-translation).
- Wrap the low-salary case in `WHEN EXCEPTION low_salary DO` inside a caller and confirm the error no longer reaches the client.
- Add `WHERE new_salary > 6000` to the `raises(10)` query: the filter composes over the SUSPENDed stream — a procedure used as a view.
- A quirk found while writing the sample: calling the failing `EXECUTE PROCEDURE` through `IAttachment::execute` (execute-immediate) with *no output metadata* returns success to this client even though the row is rejected — the prepared path raises correctly. Reproduce it, then trace where the status is lost.
- Read `RDB$PROCEDURES.RDB$PROCEDURE_BLR` for `raises` with the [BLR sample](blr-intermediate-language.md#hands-on-samples-tests-and-debugging) and find the `blr_stall` opcode — `SUSPEND` in its stored form.

### Debugging this in C++ (gdb)

With a [debug build of the engine](debugging-firebird.md), PSQL execution is a set of `StmtNode::execute` methods dispatched by the EXE looper:

```gdb
break EXE_start                        # src/jrd/exe.cpp:1185 — a request (proc/trigger body) activated
break Jrd::ExecProcedureNode::execute  # src/dsql/StmtNodes.cpp:4315 — EXECUTE PROCEDURE entering hire
break Jrd::SuspendNode::execute        # src/dsql/StmtNodes.cpp:10359 — each SUSPEND in raises
break Jrd::ExceptionNode::execute      # src/dsql/StmtNodes.cpp:5866 — EXCEPTION low_salary raised
break EXE_execute_triggers             # src/jrd/exe.cpp:1346 — emp_bi fired around the INSERT
break stuff_stack_trace                # src/jrd/exe.cpp:1668 — the "At procedure ... line:" being built
```

Fetching from `raises(10)` stops in `SuspendNode::execute` once per row, and the backtrace shows the caller's fetch pulling the procedure like any record source — the selectable-procedure design visible as a call stack. The failing `hire` stops in `ExceptionNode::execute`, then in `stuff_stack_trace`, where the line/column pair the client later prints is looked up from the debug info stored beside the procedure's BLR; `p request->getStatement()->sqlText` at `EXE_start` shows which body is running when triggers and procedures nest. The [debugging guide](debugging-firebird.md) covers attaching to the engine serving the sample.

## Further research

**Firebird**

- [`doc/sql.extensions/`](https://github.com/FirebirdSQL/firebird/tree/master/doc/sql.extensions) — the PSQL feature set, including [`README.packages.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.packages.txt), [`README.execute_block`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.execute_block), [`README.exception_handling`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.exception_handling), [`README.universal_triggers`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.universal_triggers), [`README.ddl_triggers.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.ddl_triggers.txt), [`README.db_triggers.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.db_triggers.txt), [`README.subroutines.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.subroutines.txt), [`README.autonomous_transactions.txt`](https://github.com/FirebirdSQL/firebird/blob/master/doc/sql.extensions/README.autonomous_transactions.txt).
- The [SQL dialect and data types document](sql-dialect-and-types.md) and the [transactions document](transactions-and-concurrency.md) for the types and transaction semantics PSQL uses.

**PostgreSQL**

- [PL/pgSQL](https://www.postgresql.org/docs/current/plpgsql.html), [Control structures](https://www.postgresql.org/docs/current/plpgsql-control-structures.html), [`CREATE PROCEDURE`](https://www.postgresql.org/docs/current/sql-createprocedure.html), [Triggers](https://www.postgresql.org/docs/current/triggers.html), [Event triggers](https://www.postgresql.org/docs/current/event-triggers.html), [Server programming](https://www.postgresql.org/docs/current/xproc.html).

**MySQL**

- [Stored routines](https://dev.mysql.com/doc/refman/8.4/en/stored-routines.html), [`CREATE PROCEDURE`](https://dev.mysql.com/doc/refman/8.4/en/create-procedure.html), [Triggers](https://dev.mysql.com/doc/refman/8.4/en/triggers.html), [`DECLARE ... HANDLER`](https://dev.mysql.com/doc/refman/8.4/en/declare-handler.html); MariaDB's [stored procedures](https://mariadb.com/kb/en/stored-procedures/).

**SQLite**

- [`CREATE TRIGGER`](https://sqlite.org/lang_createtrigger.html), [Application-defined functions](https://sqlite.org/appfunc.html), [Core functions](https://sqlite.org/lang_corefunc.html) — the extent of SQLite's in-database logic.
