{
  plans.pas — watching the cost-based optimizer decide, in Pascal.

  The fbintf twin of ../cpp/plans.cpp: every statement is prepared (never
  executed!) and its plan printed via IStatement.GetPlan — fbintf exposes
  the plan where the node and rust wrappers could not.  The same SELECT is
  prepared before and after CREATE INDEX so the flip from Full Scan to
  Bitmap + Index Range Scan is visible; a join with ORDER BY shows SORT on
  top of a nested loop, and an indexless equi-join shows the Firebird 5
  hash join.  One genuine fbintf gap: GetPlan hardwires the DETAILED form
  (isc getPlan(detailed = true)); the terse legacy PLAN (...) one-liner the
  C++ twin also prints is not reachable through this wrapper.
  See ../../query-optimizer-and-execution.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/plans && samples/fpc/bin/plans
}
program plans;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;

procedure ShowPlan(const sql: AnsiString);
var S: IStatement;
    plan: AnsiString;
begin
  writeln('== ', sql);
  S := A.Prepare(Tr, sql);
  plan := S.GetPlan;
  while (plan <> '') and ((plan[1] = #10) or (plan[1] = #13)) do
    Delete(plan, 1, 1);
  writeln('detailed:');
  writeln(plan);
  writeln;
end;

begin
  A := AttachOrCreate(DbConn('plans'));

  { -- Build the schema: 20 departments, 2000 employees. ------------------ }
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(Tr, 'RECREATE TABLE dept (id INT NOT NULL PRIMARY KEY,' +
    ' name VARCHAR(20))', []);
  A.ExecuteSQL(Tr, 'RECREATE TABLE emp (id INT NOT NULL PRIMARY KEY,' +
    ' dept_id INT, salary INT, name VARCHAR(20))', []);
  Tr.CommitRetaining;
  A.ExecuteSQL(Tr,
    'EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN'#10 +
    '  WHILE (i <= 20) DO BEGIN'#10 +
    '    INSERT INTO dept VALUES (:i, ''dept '' || :i); i = i + 1;'#10 +
    '  END'#10 +
    '  i = 1;'#10 +
    '  WHILE (i <= 2000) DO BEGIN'#10 +
    '    INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,'#10 +
    '        1000 + MOD(:i * 37, 500), ''emp '' || :i); i = i + 1;'#10 +
    '  END'#10 +
    'END', []);
  Tr.Commit;

  writeln('(fbintf note: IStatement.GetPlan always returns the detailed');
  writeln(' record-source tree; the legacy PLAN (...) form is not exposed)');
  writeln;

  { -- 1. No index on dept_id yet: the full scan is the only path. -------- }
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  ShowPlan('SELECT name FROM emp WHERE dept_id = 5');

  { -- 2. Create the index; the same text now compiles differently. ------- }
  A.ExecuteSQL(Tr, 'CREATE INDEX emp_dept ON emp (dept_id)', []);
  Tr.Commit;
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('-- CREATE INDEX emp_dept ON emp (dept_id) --');
  writeln;
  ShowPlan('SELECT name FROM emp WHERE dept_id = 5');

  { -- 3. PK equality: unique index, nothing cheaper than one row. -------- }
  ShowPlan('SELECT name FROM emp WHERE id = 42');

  { -- 4. Join + ORDER BY: SORT over a nested loop with the index. -------- }
  ShowPlan('SELECT e.name, d.name FROM emp e' +
    ' JOIN dept d ON e.dept_id = d.id ORDER BY e.salary');

  { -- 5. Equi-join with no usable index on either side: hash join. ------- }
  ShowPlan('SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary');

  Tr.Commit;
  writeln('done.');
end.
