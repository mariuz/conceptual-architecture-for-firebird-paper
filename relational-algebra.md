# Relational Algebra in Firebird SQL

[Relational algebra](https://en.wikipedia.org/wiki/Relational_algebra) is the small set of operators Codd defined over relations: selection, projection, rename, union, difference, product and the joins built from them. It is the mathematics underneath SQL, and it is also the shape of a query *plan*. Every operator has a Firebird SQL spelling, and most have a record source in the engine that executes exactly that operator. This document goes through the operators one by one. Each gets its SQL in Firebird 6, the plan the engine chooses for it, and the places where SQL — and Firebird in particular — departs from the algebra.

Every query and plan below was run against the demo `EMPLOYEE` database on Firebird 6 (`LI-T6.0.0.2182`), with `SET EXPLAIN ON` in isql. The whole set is one runnable script, [`samples/sql/relational_algebra.sql`](samples/sql/relational_algebra.sql). It pairs with the [query optimizer document](query-optimizer-and-execution.md) (how the record-source tree is chosen and run) and the [aggregate and window functions document](aggregate-and-window-functions.md) (the extended operators in depth).

**Table of Contents**

* [Relations and SQL tables: three departures](#relations-and-sql-tables-three-departures)
* [The operators at a glance](#the-operators-at-a-glance)
* [Selection σ](#selection-σ)
* [Projection π, duplicate elimination δ and rename ρ](#projection-π-duplicate-elimination-δ-and-rename-ρ)
* [Set operations: union yes, intersection and difference no](#set-operations-union-yes-intersection-and-difference-no)
* [Product and joins](#product-and-joins)
* [Semijoin and antijoin](#semijoin-and-antijoin)
* [Division ÷](#division-)
* [Outer joins](#outer-joins)
* [Extended operators: grouping γ and sorting τ](#extended-operators-grouping-γ-and-sorting-τ)
* [Beyond the algebra: transitive closure](#beyond-the-algebra-transitive-closure)
* [Algebraic equivalences the optimizer uses](#algebraic-equivalences-the-optimizer-uses)
* [Comparison: PostgreSQL, MySQL, SQLite](#comparison-postgresql-mysql-sqlite)
* [Discussion](#discussion)
* [Hands-on: the script, things to try, debugging](#hands-on-the-script-things-to-try-debugging)
* [Further research](#further-research)

## Relations and SQL tables: three departures

The algebra is defined over *relations*: sets of tuples, each attribute named, no order. A Firebird table is not quite that, and the differences decide how every operator below behaves.

1. **Bags, not sets.** A table may hold the same row twice, and a SQL query keeps duplicates unless told otherwise. `SELECT job_code FROM employee` is a *bag* projection; only `SELECT DISTINCT` is the algebra's π. The same split runs through union (`UNION` vs `UNION ALL`).
2. **NULL and three-valued logic.** A comparison with NULL is *unknown*, and a `WHERE` keeps only rows whose condition is *true*. The algebra has no unknown, so equivalences that hold for relations can fail for tables. The sharpest case in this document is `NOT IN` against `NOT EXISTS`, [below](#semijoin-and-antijoin).
3. **Column order and duplicate names.** A SQL result has an order of columns, and it may carry two columns with the same name (`SELECT e.dept_no, d.dept_no ...`). A relation has neither. Rename ρ is how the algebra repairs name clashes; SQL uses aliases.

## The operators at a glance

| Operator | Notation | Firebird SQL | Record source in the plan |
|---|---|---|---|
| Selection | σ<sub>p</sub>(R) | `WHERE p` | `Filter`, or an index `Range Scan` / `Unique Scan` |
| Projection | π<sub>a,b</sub>(R) | `SELECT DISTINCT a, b` | `Unique Sort` (the bag form needs no node) |
| Rename | ρ<sub>x</sub>(R) | `AS` aliases | none: names are a compile-time matter |
| Union | R ∪ S | `UNION` / `UNION ALL` | `Union`, under `Unique Sort` for `UNION` |
| Intersection | R ∩ S | no `INTERSECT`: `EXISTS` | `Hash Join (semi)` / `Nested Loop Join (semi)` |
| Difference | R − S | no `EXCEPT`: `NOT EXISTS` | a correlated `Sub-query` under a `Filter` |
| Product | R × S | `CROSS JOIN` | `Nested Loop Join (inner)` |
| Natural join | R ⋈ S | `NATURAL JOIN`, `JOIN ... USING` | `Hash Join (inner)` or `Nested Loop Join (inner)` |
| θ-join | R ⋈<sub>θ</sub> S | `JOIN ... ON θ` | a join on θ's equalities, `Filter` for the rest |
| Semijoin | R ⋉ S | `EXISTS`, `IN` | `Hash Join (semi)` / `Nested Loop Join (semi)` |
| Antijoin | R ▷ S | `NOT EXISTS` | a correlated `Sub-query` (no anti join is generated) |
| Division | R ÷ S | double `NOT EXISTS`, or `GROUP BY ... HAVING COUNT` | nested sub-queries, or `Aggregate` over a join |
| Left / right outer join | R ⟕ S, R ⟖ S | `LEFT JOIN`, `RIGHT JOIN` | `Nested Loop Join (outer)` — a right join is the mirrored left |
| Full outer join | R ⟗ S | `FULL JOIN` | `Full Outer Join` over two outer joins |
| Grouping | γ<sub>g; agg</sub>(R) | `GROUP BY`, aggregates | `Aggregate`, over a `Sort` or an index walk |
| Sorting | τ<sub>a</sub>(R) | `ORDER BY` | `Sort` (+ `Refetch`), or an index walk |
| Transitive closure | — | `WITH RECURSIVE` | `Recursion` |

## Selection σ

σ<sub>dept_no='621'</sub>(EMPLOYEE) keeps the employees of one department:

```sql
select emp_no, last_name from employee where dept_no = '621';
```

```text
Select Expression
    -> Filter
        -> Table "PUBLIC"."EMPLOYEE" Access By ID
            -> Bitmap
                -> Index "PUBLIC"."RDB$FOREIGN8" Range Scan (full match)
```

Four rows come back (Young, Ramanathan, Bishop, Green). The selection appears twice in the plan, and that is the first lesson about how the engine treats σ. The index range scan does the selecting: it builds a bitmap of the record numbers whose key equals `'621'`. The `Filter` above it re-evaluates the predicate on every fetched record, because an index key is not the value itself (collation, padding and record versions can differ). σ is "the index, then the check", never the index alone.

## Projection π, duplicate elimination δ and rename ρ

SQL projection keeps duplicates. The algebra's π is `DISTINCT`:

```sql
select count(*), count(distinct job_code) from employee;   -- 42 rows, 13 distinct job codes
select distinct job_code from employee rows 3;
```

```text
Select Expression
    -> First N Records
        -> Unique Sort (record length: 38, key length: 12)
            -> Table "PUBLIC"."EMPLOYEE" Full Scan
```

Duplicate elimination is a sort: `Unique Sort` orders the projected rows by the whole projected row and drops neighbours that are equal. Extended relational algebra writes this as a separate operator δ, so that π can stay a bag operator the way SQL uses it. Firebird's plan agrees with that split: the bag projection has no node of its own (it happens as the rows are delivered), and δ is the sort.

Rename ρ has no record source at all. `select e.emp_no as id, e.last_name as surname from employee e` changes names in the statement's description, not in any row. The table alias `e` is the algebra's ρ<sub>e</sub>(EMPLOYEE). It matters most in a self-join, where the same relation must appear under two names (the [division](#division-) and [intersection](#set-operations-union-yes-intersection-and-difference-no) examples use it).

## Set operations: union yes, intersection and difference no

Union exists in both forms:

```sql
select count(*) from (select dept_no from employee union select dept_no from department);      -- 21
select count(*) from (select dept_no from employee union all select dept_no from department);  -- 63
```

```text
Select Expression                              Select Expression
    -> Aggregate                                   -> Aggregate
        -> Unique Sort (record length: 54, ...)        -> Union
            -> Union                                       -> Table "PUBLIC"."EMPLOYEE" Full Scan
                -> Table "PUBLIC"."EMPLOYEE" ...           -> Table "PUBLIC"."DEPARTMENT" Full Scan
                -> Table "PUBLIC"."DEPARTMENT" ...
```

`UNION ALL` is the bag union, a `Union` record source that reads its branches one after the other (42 + 21 = 63). `UNION` is the set union: the same `Union` under a `Unique Sort` that removes duplicates (21, since every employee's department is also a department).

**Firebird has no `INTERSECT` and no `EXCEPT`**, not even in Firebird 6. Both fail to parse. `EXCEPT` is not even a reserved word: in `select dept_no from department except select ...` the parser takes `except` as a table alias and then stumbles on the `select` after it (`Token unknown - select`). Both operations are therefore written with subqueries:

- **R ∩ S** is a semijoin on the whole row: the rows of R for which a matching row in S exists. Employees on project VBASE *and* on project MAPDB:

  ```sql
  select p.emp_no from employee_project p where p.proj_id = 'VBASE'
    and exists (select 1 from employee_project q where q.emp_no = p.emp_no and q.proj_id = 'MAPDB');
  ```

  ```text
  Select Expression
      -> Nested Loop Join (semi)
          -> Filter
              -> Table "PUBLIC"."EMPLOYEE_PROJECT" as "P" Access By ID
                  -> Bitmap
                      -> Index "PUBLIC"."RDB$FOREIGN16" Range Scan (full match)
          -> Filter
              -> Table "PUBLIC"."EMPLOYEE_PROJECT" as "Q" Access By ID
                  -> Bitmap
                      -> Index "PUBLIC"."RDB$PRIMARY14" Unique Scan
  ```

  The answer is employees 71 and 4. The `EXISTS` became a real join operator, a semi join, [described next](#semijoin-and-antijoin).
- **R − S** is an antijoin: `NOT EXISTS`. That one stays a subquery, also described below.

Both rewrites are exact only when no column involved can be NULL. `INTERSECT` and `EXCEPT` compare rows as *distinct-from* (two NULLs match), while `=` inside an `EXISTS` never matches a NULL. Where a column is nullable, write the condition with `IS NOT DISTINCT FROM`.

## Product and joins

The Cartesian product R × S is `CROSS JOIN`. With 21 departments and 31 jobs:

```sql
select count(*) from department cross join job;   -- 651
```

```text
Select Expression
    -> Aggregate
        -> Nested Loop Join (inner)
            -> Table "PUBLIC"."DEPARTMENT" Full Scan
            -> Table "PUBLIC"."JOB" Full Scan
```

Every join in the algebra is defined as a selection over a product, R ⋈<sub>θ</sub> S = σ<sub>θ</sub>(R × S). No engine executes it that way; the point of a join operator is to avoid building the product. The natural join and its `USING` spelling compile to the same plan, a hash join on the shared column `DEPT_NO`:

```sql
select count(*) from employee natural join department;            -- 42
select count(*) from employee join department using (dept_no);   -- 42, same plan
```

```text
Select Expression
    -> Aggregate
        -> Filter
            -> Hash Join (inner) (keys: 1, total key length: 3)
                -> Table "PUBLIC"."EMPLOYEE" Full Scan
                -> Record Buffer (record length: 25)
                    -> Table "PUBLIC"."DEPARTMENT" Full Scan
```

The smaller relation (DEPARTMENT) is buffered and hashed on the join key, and the larger one probes it. A θ-join whose condition mixes an equality with a range shows the definition σ<sub>θ</sub>(R × S) being taken apart:

```sql
select count(*) from employee e join job j
  on e.salary between j.min_salary and j.max_salary and e.job_country = j.job_country;   -- 263
```

```text
Select Expression
    -> Aggregate
        -> Filter
            -> Hash Join (inner) (keys: 1, total key length: 15)
                -> Table "PUBLIC"."EMPLOYEE" as "E" Full Scan
                -> Record Buffer (record length: 57)
                    -> Table "PUBLIC"."JOB" as "J" Full Scan
```

The optimizer splits θ into its conjuncts. The equality `e.job_country = j.job_country` becomes the hash key (15 bytes: a country name), and the `BETWEEN` stays a `Filter` above the join. In algebra this is the rewrite σ<sub>θ1 ∧ θ2</sub>(R × S) = σ<sub>θ2</sub>(R ⋈<sub>θ1</sub> S): only an equality can be hashed, so everything else becomes a selection on the join's output. The result is larger than EMPLOYEE (263 rows for 42 employees) because a salary falls inside the range of several jobs in the same country.

## Semijoin and antijoin

A semijoin R ⋉ S keeps the rows of R that have at least one partner in S, and returns each of them once, however many partners it has. SQL spells it `EXISTS` or `IN`, and in Firebird 6 both become a real join operator:

```sql
select count(*) from employee e where exists (select 1 from employee_project p where p.emp_no = e.emp_no);   -- 22
select count(*) from department d where d.dept_no in (select e.dept_no from employee e);                   -- 19
```

```text
Select Expression
    -> Aggregate
        -> Filter
            -> Hash Join (semi) (keys: 1, total key length: 2)
                -> Table "PUBLIC"."EMPLOYEE" as "E" Full Scan
                -> Record Buffer (record length: 25)
                    -> Table "PUBLIC"."EMPLOYEE_PROJECT" as "P" Full Scan
```

The conversion happens before optimization, in `findPossibleJoins` ([`src/jrd/RecordSourceNodes.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/RecordSourceNodes.cpp), around line 136). It walks the `WHERE` clause's conjuncts and picks out `blr_any` (`EXISTS`) and `blr_ansi_any` (`IN`) subqueries whose correlation can serve as a join condition. Each one becomes a `SEMI_JOIN` stream, and the optimizer then joins it like any other stream ([`Optimizer.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/optimizer/Optimizer.cpp) around line 1099). It joins with a hash when no index fits, and with a nested loop when one does, as in the [intersection](#set-operations-union-yes-intersection-and-difference-no) above. Two limits are visible in the source. A subquery with `FIRST`, `SKIP` or a `PLAN` is not converted. And a subquery nested *inside* another subquery is left alone, with a comment that calls this a temporary fix until nested-loop semi-joins are allowed there.

The antijoin R ▷ S keeps the rows of R that have *no* partner in S. The record sources already implement it: `NestedLoopJoin.cpp` and `HashJoin.cpp` both handle `JoinType::ANTI`. The conversion never produces one, though. `findPossibleJoins` pushes every candidate with `semiJoin = true` (line 213), and it only looks at `EXISTS` and `IN`, so a `NOT EXISTS` stays a correlated subquery that runs once per outer row:

```sql
select d.dept_no, d.department from department d
 where not exists (select 1 from employee e where e.dept_no = d.dept_no);
```

```text
Sub-query
    -> Filter
        -> Table "PUBLIC"."EMPLOYEE" as "E" Access By ID
            -> Bitmap
                -> Index "PUBLIC"."RDB$FOREIGN8" Range Scan (full match)
Select Expression
    -> Filter
        -> Table "PUBLIC"."DEPARTMENT" as "D" Full Scan
```

This is the difference DEPARTMENT − π<sub>dept_no</sub>(EMPLOYEE): departments 620 and 116 have no employees. With the foreign-key index on `EMPLOYEE.DEPT_NO`, the per-row probe is cheap. Without an index, each of the 21 departments would scan EMPLOYEE.

**`NOT IN` is not the antijoin.** Under three-valued logic, `x NOT IN (subquery)` is *unknown* as soon as the subquery yields a NULL, so a single NULL removes every row:

```sql
select count(*) from department d where d.dept_no not in (select h.head_dept from department h);   -- 0
select count(*) from department d where not exists
  (select 1 from department h where h.head_dept = d.dept_no);                                      -- 14
```

The head office's `HEAD_DEPT` is NULL, so `NOT IN` returns no departments at all. `NOT EXISTS` returns the 14 departments that head no other department, which is the algebra's answer. When the subquery's column can be NULL, `NOT EXISTS` is the antijoin, and `NOT IN` is something else.

## Division ÷

Division answers "for all" questions. R(emp, proj) ÷ S(proj) is the set of employees assigned to *every* project in S. SQL has no division operator. The classic spelling is a double negation, "there is no project in S that this employee is not on". Here S is the software projects that have a team leader (VBASE and MAPDB):

```sql
select e.emp_no, e.last_name from employee e
 where not exists (select 1 from project s where s.product = 'software' and s.team_leader is not null
   and not exists (select 1 from employee_project r where r.emp_no = e.emp_no and r.proj_id = s.proj_id));
```

```text
Sub-query
    -> Filter
        -> Table "PUBLIC"."EMPLOYEE_PROJECT" as "R" Access By ID
            -> Bitmap
                -> Index "PUBLIC"."RDB$PRIMARY14" Unique Scan
Sub-query
    -> Filter
        -> Table "PUBLIC"."PROJECT" as "S" Access By ID
            -> Bitmap
                -> Index "PUBLIC"."PRODTYPEX" Range Scan (partial match: 1/2)
Select Expression
    -> Filter
        -> Table "PUBLIC"."EMPLOYEE" as "E" Full Scan
```

The answer is Young (4) and Burbank (71). Both `NOT EXISTS` levels stay nested correlated subqueries, for the antijoin reason above. The counting form gives the same answer with a join and an aggregate:

```sql
select r.emp_no from employee_project r join project s on s.proj_id = r.proj_id
 where s.product = 'software' and s.team_leader is not null
 group by r.emp_no
 having count(*) = (select count(*) from project where product = 'software' and team_leader is not null);
```

```text
Sub-query (invariant)
    -> Singularity Check
        -> Aggregate
            -> Filter
                -> Table "PUBLIC"."PROJECT" Access By ID
                    -> Bitmap
                        -> Index "PUBLIC"."PRODTYPEX" Range Scan (partial match: 1/2)
Select Expression
    -> Filter
        -> Aggregate
            -> Sort (record length: 76, key length: 8)
                -> Nested Loop Join (inner)
                    -> Filter
                        -> Table "PUBLIC"."PROJECT" as "S" Access By ID ...
                    -> Filter
                        -> Table "PUBLIC"."EMPLOYEE_PROJECT" as "R" Access By ID ...
```

The size of S is computed once (`Sub-query (invariant)`), and each employee's count of matching projects is compared with it. The counting form is only correct when (emp, proj) is unique in R — here it is the primary key of EMPLOYEE_PROJECT — and when S is not empty. For an empty S the double negation returns every employee, which is the algebra's answer, while the counting form returns none.

## Outer joins

The outer joins are not in Codd's original algebra; they were added to keep the rows a join would lose, padded with NULLs. A left join keeps every department, including the two with no employees (42 matches + 2 = 44 rows):

```sql
select count(*) from department d left join employee e on e.dept_no = d.dept_no;   -- 44
select count(*) from employee e right join department d on e.dept_no = d.dept_no;  -- 44
```

Both statements get the **same** plan, with DEPARTMENT as the outer stream:

```text
Select Expression
    -> Aggregate
        -> Nested Loop Join (outer)
            -> Table "PUBLIC"."DEPARTMENT" as "D" Full Scan
            -> Filter
                -> Table "PUBLIC"."EMPLOYEE" as "E" Access By ID
                    -> Bitmap
                        -> Index "PUBLIC"."RDB$FOREIGN8" Range Scan (full match)
```

There is no right-outer-join operator in the engine. R ⟖ S is compiled as S ⟕ R, the algebraic identity, with the two sides swapped. The full outer join has its own record source, built from two left joins:

```sql
select count(*) from employee e full join project p on p.team_leader = e.emp_no;   -- 43
```

```text
Select Expression
    -> Aggregate
        -> Full Outer Join
            -> Nested Loop Join (outer)
                -> Table "PUBLIC"."EMPLOYEE" as "E" Full Scan
                -> Filter
                    -> Table "PUBLIC"."PROJECT" as "P" Access By ID ...
            -> Nested Loop Join (outer)
                -> Table "PUBLIC"."PROJECT" as "P" Full Scan
                -> Filter
                    -> Table "PUBLIC"."EMPLOYEE" as "E" Access By ID ...
```

[`FullOuterJoin.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/recsrc/FullOuterJoin.cpp) (`internalGetRecord`) returns every row of the first branch, R ⟕ S. It then reads the second branch, S ⟕ R, and keeps only the rows whose R side did *not* match, because the matching ones were already returned by the first branch. In algebra, R ⟗ S = (R ⟕ S) ∪ (S ▷ R) padded with NULLs, and the second branch is an antijoin done by hand. The count is 42 employees + the one project without a team leader (HWRII) = 43.

## Extended operators: grouping γ and sorting τ

Extended relational algebra adds grouping with aggregation, γ<sub>g; f(a)</sub>(R), and sorting, τ<sub>a</sub>(R). Neither is defined over pure sets: γ produces new values, and τ produces a list rather than a set.

```sql
select dept_no, count(*), avg(salary) from employee group by dept_no having count(*) > 4;
```

```text
Select Expression
    -> Filter
        -> Aggregate
            -> Table "PUBLIC"."EMPLOYEE" Access By ID
                -> Index "PUBLIC"."RDB$FOREIGN8" Full Scan
```

Only department 623 has more than four employees (5, average salary 57551.65). `HAVING` is a selection σ on γ's output, which is why the `Filter` sits *above* the `Aggregate`. `Aggregate` needs its input grouped. Here it gets that without a sort, by walking the foreign-key index on `DEPT_NO` in key order (`Index ... Full Scan` under `Access By ID`). The [aggregate and window functions document](aggregate-and-window-functions.md) covers the rest of γ.

τ is a `Sort`, and with `FIRST` it only has to produce the top rows:

```sql
select first 3 last_name, salary from employee order by salary desc;   -- Yamamoto, Ichida, Bender
```

```text
Select Expression
    -> First N Records
        -> Refetch
            -> Sort (record length: 36, key length: 12)
                -> Table "PUBLIC"."EMPLOYEE" Full Scan
```

The `Sort` carries only the key and the record's address. `Refetch` reads each delivered record again from the table, so wide rows don't pass through the sort, as the [sorting and temp space document](sorting-and-temp-space.md) explains.

## Beyond the algebra: transitive closure

Some questions cannot be written in relational algebra at all. The best known is transitive closure: "all departments under Engineering, at any depth". Every algebra expression has a fixed number of joins, so no expression can follow a hierarchy of arbitrary depth. SQL added recursion for this, and Firebird has its own record source for it:

```sql
with recursive sub (dept_no, department, lvl) as (
  select dept_no, department, 0 from department where dept_no = '600'
  union all
  select d.dept_no, d.department, s.lvl + 1 from department d join sub s on d.head_dept = s.dept_no)
select dept_no, department, lvl from sub;
```

```text
Select Expression
    -> Recursion
        -> Filter
            -> Table "PUBLIC"."DEPARTMENT" as "SUB" "PUBLIC"."DEPARTMENT" Access By ID
                -> Bitmap
                    -> Index "PUBLIC"."RDB$PRIMARY5" Unique Scan
        -> Filter
            -> Table "PUBLIC"."DEPARTMENT" as "SUB" "D" Access By ID
                -> Bitmap
                    -> Index "PUBLIC"."RDB$FOREIGN6" Range Scan (full match)
```

The result is eight departments: Engineering at level 0, its two divisions at level 1, and five departments at level 2. `Recursion` runs the anchor branch once, then repeatedly runs the recursive branch against the rows of the previous level. Each step is an ordinary join (here an index probe on `HEAD_DEPT`), and recursion stops when a level produces no rows.

## Algebraic equivalences the optimizer uses

Query optimization rests on the fact that different algebra expressions can describe the same relation. The optimizer picks the one that is cheapest to execute. Three equivalences can be seen directly in Firebird's plans:

- **Selection pushdown**: σ<sub>p</sub>(R ⋈ S) = σ<sub>p</sub>(R) ⋈ S when p mentions only R. Here both selections are evaluated on their own stream, below the join, instead of on the joined rows:

  ```sql
  select count(*) from employee e join department d on d.dept_no = e.dept_no
   where d.location = 'Monterey' and e.salary > 50000;   -- 14
  ```

  ```text
  Select Expression
      -> Aggregate
          -> Nested Loop Join (inner)
              -> Filter
                  -> Table "PUBLIC"."DEPARTMENT" as "D" Full Scan
              -> Filter
                  -> Table "PUBLIC"."EMPLOYEE" as "E" Access By ID
                      -> Bitmap
                          -> Index "PUBLIC"."RDB$FOREIGN8" Range Scan (full match)
  ```

  The `Filter` over DEPARTMENT applies the location test before any employee is read. The `Filter` over EMPLOYEE applies both the join condition (through the index) and the salary test.
- **Join commutativity and associativity**: R ⋈ S = S ⋈ R, and (R ⋈ S) ⋈ T = R ⋈ (S ⋈ T). Together they let the optimizer choose any join order. Above, `employee e join department d` runs with DEPARTMENT outer. The right-join case shows the same freedom used for outer joins: R ⟖ S is executed as S ⟕ R.
- **Subquery to join**: σ<sub>∃S</sub>(R) = R ⋉ S. This is the conversion of `EXISTS` and `IN` into semi joins. It is the newest of the three in Firebird, and the [semijoin section](#semijoin-and-antijoin) shows where it currently stops: at the antijoin.

The [query optimizer document](query-optimizer-and-execution.md) explains how the cost model picks among the equivalent forms.

## Comparison: PostgreSQL, MySQL, SQLite

| | Firebird 6 | PostgreSQL | MySQL 8 | SQLite |
|---|---|---|---|---|
| `INTERSECT` / `EXCEPT` | no | yes (with `ALL` variants) | yes, since 8.0.31 | yes (no `ALL` variants) |
| `NATURAL JOIN`, `USING` | yes | yes | yes | yes |
| `FULL OUTER JOIN` | yes | yes | no | yes, since 3.39 |
| `EXISTS` / `IN` as a semi join | yes (hash or nested loop) | yes (`Semi Join`) | yes (semijoin strategies) | no: subqueries, or an automatic index |
| `NOT EXISTS` as an anti join | no (correlated subquery) | yes (`Anti Join`) | yes, since 8.0.17 | no |
| Hash join | yes | yes | yes, since 8.0.18 | no |
| Recursive queries | `WITH RECURSIVE` | `WITH RECURSIVE` | `WITH RECURSIVE` | `WITH RECURSIVE` |

The PostgreSQL, MySQL and SQLite columns come from their documentation (see [Further research](#further-research)) and were not measured here; the Firebird column was.

## Discussion

Firebird's executor lines up closely with the algebra. Union, product, semijoin, outer joins, grouping, sorting and recursion each have a record source of their own, and an explained plan reads almost like the algebra expression it executes. The gaps are on the SQL side, and they concentrate on difference. There is no `INTERSECT` and no `EXCEPT`, and the `NOT EXISTS` that has to stand in for `EXCEPT` is never turned into the anti join the record sources already implement. That costs little with an index on the correlated column. Without one, it means one subquery run per outer row. The `NOT IN` pitfall makes the same point from the other side: in the presence of NULL, SQL's negations are not the algebra's, and the choice between `NOT IN` and `NOT EXISTS` decides the answer, not just the speed.

## Hands-on: the script, things to try, debugging

### SQL script — [`samples/sql/relational_algebra.sql`](samples/sql/relational_algebra.sql)

Every query in this document, in order, read-only against the demo database, each printed with its explained plan:

```sh
isql -user SYSDBA -password masterkey -q inet://localhost/employee \
     -i samples/sql/relational_algebra.sql
```

Verified on Firebird 6 (`LI-T6.0.0.2182`): all 25 statements run, and their plans and results are the ones quoted above.

### Things to try

- Replace `set explain on` with `set plan on` to see the legacy one-line plans, which hide most of the algebra. The semi join prints as a plain `PLAN HASH ("E" NATURAL, "P" NATURAL)`, the same as an inner hash join. The full join prints as `PLAN JOIN (JOIN (...), JOIN (...))`. The recursion has no keyword at all.
- Drop the foreign key's index from a copy of the database (`alter table employee drop constraint ...` on a scratch copy) and re-run the `NOT EXISTS` difference. Each department now scans EMPLOYEE, which is what a missing anti join costs.
- Add a NULL `dept_no` employee to a scratch copy and compare `NOT IN` with `NOT EXISTS` against EMPLOYEE.
- Write the division for an empty S (`s.product = 'firmware'`) in both forms and see the double negation return every employee and the counting form return none.
- Nest the intersection's `EXISTS` inside another subquery and watch the semi join disappear again.

### Debugging this in C++ (gdb)

With a [debug build of the engine](debugging-firebird.md):

```gdb
break findPossibleJoins          # src/jrd/RecordSourceNodes.cpp - EXISTS / IN found and turned into SEMI_JOIN
break FullOuterJoin::internalGetRecord   # src/jrd/recsrc/FullOuterJoin.cpp - the two passes of R full join S
break HashJoin::internalGetRecord        # src/jrd/recsrc/HashJoin.cpp - inner, outer and semi probes
```

Run the semijoin query from the script and the first breakpoint fires during compilation, before the optimizer sees the statement. Stepping through it on the `NOT EXISTS` query shows why nothing is converted there: only `blr_any` and `blr_ansi_any` subqueries qualify.

## Further research

**Relational algebra**

- [Relational algebra](https://en.wikipedia.org/wiki/Relational_algebra) (Wikipedia): the operators, their notation, and the extended operators used in this document.
- E. F. Codd, ["A Relational Model of Data for Large Shared Data Banks"](https://dl.acm.org/doi/10.1145/362384.362685), *Communications of the ACM* 13(6), 1970: the paper that introduced the model.
- [Codd's theorem](https://en.wikipedia.org/wiki/Codd%27s_theorem): relational algebra and the relational calculus have the same expressive power.

**Firebird**

- [Firebird 5.0 Language Reference](https://firebirdsql.org/file/documentation/html/en/refdocs/fblangref50/firebird-50-language-reference.html): `SELECT`, joins, `UNION`, `EXISTS`, `IN`, recursive CTEs.
- Source: [`src/jrd/RecordSourceNodes.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/RecordSourceNodes.cpp) (`findPossibleJoins`), [`src/jrd/optimizer/Optimizer.cpp`](https://github.com/FirebirdSQL/firebird/blob/master/src/jrd/optimizer/Optimizer.cpp), [`src/jrd/recsrc/`](https://github.com/FirebirdSQL/firebird/tree/master/src/jrd/recsrc) (`HashJoin`, `NestedLoopJoin`, `FullOuterJoin`, `Union`, `RecursiveStream`).
- In this collection: the [query optimizer](query-optimizer-and-execution.md), [aggregate and window functions](aggregate-and-window-functions.md), [sorting and temp space](sorting-and-temp-space.md) and [BLR](blr-intermediate-language.md) documents.

**PostgreSQL, MySQL, SQLite**

- PostgreSQL: [Combining queries (UNION, INTERSECT, EXCEPT)](https://www.postgresql.org/docs/current/queries-union.html), [Using EXPLAIN](https://www.postgresql.org/docs/current/using-explain.html).
- MySQL: [INTERSECT](https://dev.mysql.com/doc/refman/8.4/en/intersect.html) and [EXCEPT](https://dev.mysql.com/doc/refman/8.4/en/except.html), [Optimizing subqueries with semijoin and antijoin transformations](https://dev.mysql.com/doc/refman/8.4/en/semijoins-antijoins.html), [Hash join optimization](https://dev.mysql.com/doc/refman/8.4/en/hash-joins.html).
- SQLite: [SELECT, including compound operators](https://sqlite.org/lang_select.html), [The query planner](https://sqlite.org/queryplanner.html), [Automatic indexes](https://sqlite.org/optoverview.html#autoindex).
