{
  metadata_cache.pas — the metadata-cache scenario in Pascal.

  The fbintf twin of ../cpp/metadata_cache.cpp: the visibility rule of
  the metadata cache, exercised from two attachments (A and B) on one
  scratch database —

    1. an uncommitted ALTER is visible to its own transaction and to
       nobody else — B gets "Column unknown" while A already selects
       the new column;
    2. once A commits, a committed version is visible to everyone
       IMMEDIATELY — even to a statement prepared inside B's older,
       still-open SNAPSHOT transaction: metadata is read-committed,
       not snapshot-isolated;
    3. two concurrent uncommitted DDLs on the same object collide —
       the engine's "object in use" error;
    4. every ALTER appended a row to RDB$FORMATS: old records decode
       lazily (NULL in columns that postdate them).

  See ../../metadata-cache.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/metadata_cache && samples/fpc/bin/metadata_cache
}
program metadata_cache;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A, B: IAttachment;
    ADdl, BTra, T: ITransaction;
    RS: IResultSet;
    i: integer;
    line: AnsiString;

function Flat(const s: AnsiString): AnsiString;
begin
  Result := StringReplace(Trim(s), #13, '', [rfReplaceAll]);
  Result := StringReplace(Result, #10, ' ', [rfReplaceAll]);
end;

{Run a query, printing either the first-row/first-column value or the error.}
procedure TryQuery(const who: AnsiString; att: IAttachment; tr: ITransaction;
  const sql: AnsiString);
var v: AnsiString;
    data: ISQLData;
begin
  try
    data := att.OpenCursorAtStart(tr, sql)[0];
    if data.IsNull then
      v := '<null>'
    else
      v := data.AsString;
  except on E: EIBInterBaseError do
    v := 'ERROR: ' + Flat(E.Message);
  end;
  writeln(who, ': ', sql, ' -> ', v);
end;

begin
  A := AttachOrCreate(DbConn('metadata_cache'));
  B := FirebirdAPI.OpenDatabase(DbConn('metadata_cache'), DefaultDPB);

  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'recreate table t (a integer)');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'insert into t values (1)');

  { -- 1. uncommitted DDL: mine, and mine alone ----------------------- }
  writeln('== 1. uncommitted ALTER: visible to creator only ==');
  ADdl := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);                              { A's DDL stays uncommitted }
  A.ExecuteSQL(ADdl, 'alter table t add e integer', []);
  TryQuery('A (same tx)  ', A, ADdl, 'select e from t');
  BTra := B.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  TryQuery('B            ', B, BTra, 'select e from t');
  BTra.Commit;

  { -- 2. committed DDL ignores open snapshots ------------------------ }
  writeln;
  writeln('== 2. committed ALTER: seen even inside B''s open SNAPSHOT tx ==');
  BTra := B.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);                              { explicit SNAPSHOT }
  TryQuery('B (snapshot) ', B, BTra, 'select count(*) from t');
  ADdl.Commit;                              { E becomes committed }
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'alter table t add d integer');         { D committed after B's snapshot }
  TryQuery('B (same  tx) ', B, BTra, 'select d from t');
  writeln('   (records are snapshot-isolated; metadata is read-committed -');
  writeln('    the new statement was prepared against the chain''s current head)');
  BTra.Commit;

  { -- 3. concurrent DDL: the newVersion collision -------------------- }
  writeln;
  writeln('== 3. two uncommitted DDLs on one object ==');
  ADdl := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(ADdl, 'alter table t add f integer', []);
  BTra := B.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taRollback);
  try
    B.ExecuteSQL(BTra, 'alter table t add g integer', []);
    writeln('B: ALTER unexpectedly succeeded');
  except on E: EIBInterBaseError do
    writeln('B: ALTER failed:'#10, E.Message);
  end;
  BTra.Rollback;
  ADdl.Rollback;                            { F vanishes with the rollback }

  { -- 4. the on-disk half: one format per committed shape ------------ }
  writeln;
  writeln('== 4. RDB$FORMATS after the committed DDL ==');
  T := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('formats stored for T: ',
    A.OpenCursorAtStart(T, 'select count(*) from rdb$formats f '
      + 'join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id '
      + 'where r.rdb$relation_name = ''T''')[0].AsString,
    ' (T has lived through that many shapes)');
  RS := A.OpenCursor(T, 'select a, e, d from t');
  writeln('A | E | D');
  while RS.FetchNext do
  begin
    line := '';
    for i := 0 to RS.getCount - 1 do
    begin
      if i > 0 then line := line + ' | ';
      if RS[i].IsNull then
        line := line + '<null>'
      else
        line := line + RS[i].AsString;
    end;
    writeln(line);
  end;
  RS.Close;
  T.Commit;

  writeln('done.');
end.
