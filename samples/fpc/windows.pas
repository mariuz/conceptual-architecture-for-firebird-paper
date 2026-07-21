{
  windows.pas — aggregate and window functions, via fbintf.

  The fbintf twin of ../cpp/windows.cpp: the document's six-row sales
  table and its flagship analytics — the ranking/frame/LAG window query,
  FILTER + LISTAGG + STDDEV_POP aggregates, PERCENTILE_CONT and a
  hypothetical-set RANK ... WITHIN GROUP, plus a Firebird 6 frame with
  EXCLUDE CURRENT ROW.  The result tables are printed generically from
  the IStatement metadata (column names) and IResultset (values), and
  IStatement.GetPlan prints the plan for the window query.  A bonus over
  the C++ twin: fbintf asks for the EXPLAINED plan, so instead of the
  one-line PLAN SORT(...) you see the whole Window Partition -> Record
  Buffer -> Sort cascade — the SortedStream -> WindowedStream pipeline
  of the document's execution section, spelled out per window.
  See ../../aggregate-and-window-functions.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/windows && samples/fpc/bin/windows
}
program windows;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const DDL_TPB: array[0..3] of byte = (isc_tpb_write, isc_tpb_nowait,
        isc_tpb_read_committed, isc_tpb_rec_version);

var Att: IAttachment;
    Tr: ITransaction;
    Stmt: IStatement;
    r: integer;

const ROWS: array[0..5] of AnsiString = (
  '(1,''East'',100)', '(2,''East'',200)', '(3,''East'',150)',
  '(4,''West'',300)', '(5,''West'',250)', '(6,''West'',400)');

const WIN_SQL =
  'SELECT region, amount,' +
  ' ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount) AS rn,' +
  ' RANK() OVER (ORDER BY amount DESC) AS overall_rank,' +
  ' SUM(amount) OVER (PARTITION BY region ORDER BY id' +
  '   ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_total,' +
  ' LAG(amount) OVER (PARTITION BY region ORDER BY id) AS prev_amount' +
  ' FROM sales';

function Pad(const s: AnsiString; w: integer): AnsiString;
begin
  Result := s;
  while Length(Result) < w do
    Result := Result + ' ';
end;

{ Run a SELECT and print it the way isql would: column names from the
  statement metadata, every value fetched as a string via ISQLData. }
procedure ShowQuery(const sql: AnsiString);
var Stmt: IStatement;
    Cur: IResultSet;
    names: array of AnsiString;
    widths: array of integer;
    rows: array of array of AnsiString;
    n, i, r: integer;
    s: AnsiString;
begin
  Stmt := Att.Prepare(Tr, sql);
  n := Stmt.MetaData.Count;
  SetLength(names, n);
  SetLength(widths, n);
  for i := 0 to n - 1 do
  begin
    names[i] := Stmt.MetaData[i].getAliasName;
    widths[i] := Length(names[i]);
  end;
  Cur := Stmt.OpenCursor;
  SetLength(rows, 0);
  while Cur.FetchNext do
  begin
    r := Length(rows);
    SetLength(rows, r + 1);
    SetLength(rows[r], n);
    for i := 0 to n - 1 do
    begin
      if Cur[i].IsNull then
        s := '<null>'
      else
        s := Trim(Cur[i].AsString);
      rows[r][i] := s;
      if Length(s) > widths[i] then
        widths[i] := Length(s);
    end;
  end;
  Cur.Close;
  for i := 0 to n - 1 do write(Pad(names[i], widths[i]), ' ');
  writeln;
  for i := 0 to n - 1 do write(StringOfChar('-', widths[i]), ' ');
  writeln;
  for r := 0 to Length(rows) - 1 do
  begin
    for i := 0 to n - 1 do write(Pad(rows[r][i], widths[i]), ' ');
    writeln;
  end;
end;

begin
  Att := AttachOrCreate(DbConn('windows'));

  try
    Att.ExecImmediate(DDL_TPB, 'DROP TABLE sales');
  except on EIBInterBaseError do ;
  end;
  Att.ExecImmediate(DDL_TPB,
    'CREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(10),' +
    ' amount NUMERIC(10,2))');

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  for r := 0 to High(ROWS) do
    Att.ExecuteSQL(Tr, 'INSERT INTO sales VALUES ' + ROWS[r], []);

  { -- 1. The flagship window query: partitioned ranking, a framed
          running total, and LAG navigation — every row kept. }
  writeln('== window functions ==');
  ShowQuery(WIN_SQL);

  { fbintf's GetPlan returns the explained plan: the per-window
    Sort -> Record Buffer -> Window Partition cascade. }
  Stmt := Att.Prepare(Tr, WIN_SQL);
  writeln;
  writeln('plan: ', Trim(Stmt.GetPlan));

  { -- 2. Aggregates: FILTER (FB5), ordered LISTAGG, statistical. }
  writeln;
  writeln('== aggregates: FILTER / LISTAGG / STDDEV_POP ==');
  ShowQuery(
    'SELECT region, COUNT(*) AS n,' +
    ' COUNT(*) FILTER (WHERE amount > 150) AS big_sales,' +
    ' CAST(LISTAGG(amount, '','') WITHIN GROUP (ORDER BY amount)' +
    '   AS VARCHAR(60)) AS amounts,' +
    ' CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev' +
    ' FROM sales GROUP BY region');

  { -- 3. Ordered-set and hypothetical-set aggregates: the median,
          and "what rank would a 175 sale have in each region?" }
  writeln;
  writeln('== PERCENTILE_CONT median / hypothetical RANK(175) ==');
  ShowQuery(
    'SELECT region,' +
    ' PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median,' +
    ' RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175' +
    ' FROM sales GROUP BY region');

  { -- 4. FB6 frame exclusion: each row's neighbours' average,
          the row itself EXCLUDEd from its own frame. }
  writeln;
  writeln('== FB6 frame EXCLUDE CURRENT ROW (neighbours'' average) ==');
  ShowQuery(
    'SELECT id, amount,' +
    ' CAST(AVG(amount) OVER (ORDER BY id' +
    '   ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING' +
    '   EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg' +
    ' FROM sales');

  Tr.Commit;
  writeln;
  writeln('done.');
end.
