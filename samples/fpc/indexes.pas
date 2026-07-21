{
  indexes.pas — one B-tree, many variants, in Pascal.

  The fbintf twin of ../cpp/indexes.cpp: builds a 3,000-row table,
  creates a descending, an expression (COMPUTED BY), a partial (WHERE)
  and a plain index, then prepares five queries and prints the
  optimizer's plan for each.  fbintf exposes the plan directly —
  IStatement.GetPlan — a call the node-firebird and rsfbclient twins
  had no equivalent for; note that fbintf asks the engine for the
  DETAILED (explained) plan form, so the output is the tree rendering
  rather than the C++ twin's legacy one-liners.  The plans prove:
  expression predicate -> expression index, matching filter -> partial
  index, ORDER BY DESC -> descending index navigation, OR -> two
  indexes bitmap-combined, and CONTAINING -> full scan — the
  no-native-full-text gap the document ends on.
  See ../../indexing-and-full-text-search.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/indexes && samples/fpc/bin/indexes
}
program indexes;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;

procedure ShowPlan(const sql: AnsiString);
var S: IStatement;
    p: AnsiString;
begin
  S := A.Prepare(Tr, sql);
  p := Trim(S.GetPlan);
  writeln(sql);
  if p = '' then writeln('(no plan)') else writeln(p);
  writeln;
end;

begin
  A := AttachOrCreate(DbConn('indexes'));

  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'recreate table doc ('
    + ' id integer, title varchar(60), status varchar(10), num integer)');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'execute block as declare i integer = 0; begin'
    + '  while (i < 3000) do begin'
    + '    insert into doc values (:i, ''Title '' || :i,'
    + '      iif(mod(:i, 3) = 0, ''active'', ''done''), mod(:i, 100));'
    + '    i = i + 1;'
    + '  end '
    + 'end');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'create descending index doc_id_desc on doc (id)');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'create index doc_upper_title on doc computed by (upper(title))');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'create index doc_active on doc (status) where status = ''active''');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'create index doc_num on doc (num)');
  writeln('3000 rows; indexes: descending, expression, partial, plain');
  writeln('(plans below are the detailed form: fbintf''s GetPlan asks for the explained plan)');
  writeln;

  Tr := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  ShowPlan('select id from doc where upper(title) = ''TITLE 5''');
  ShowPlan('select id from doc where status = ''active''');
  ShowPlan('select first 1 id from doc order by id desc');
  ShowPlan('select id from doc where num = 42 or id = 7');
  ShowPlan('select id from doc where title containing ''itle 12''');

  writeln('CONTAINING is correct but unindexed: matched ',
    A.OpenCursorAtStart(Tr,
      'select count(*) from doc where title containing ''itle 12''')[0].AsString,
    ' rows by scanning all 3000');
  Tr.Commit;

  writeln('done.');
end.
