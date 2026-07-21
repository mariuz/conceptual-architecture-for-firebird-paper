{
  extensibility.pas — calling native code through SQL: UDR end to end.

  The fbintf twin of ../cpp/extensibility.cpp: the shipped example UDR
  module (plugins/udr/libudrcpp_example.so) is bound to SQL objects
  with EXTERNAL NAME '<module>!<entry>' ENGINE udr, then called like
  any other procedure/function — the whole extension path of the
  document's Figure 2: engine -> udr_engine plugin
  (TYPE_EXTERNAL_ENGINE) -> native module.  The sample also shows the
  SQL-visible surface of the plugin architecture: RDB$CONFIG names the
  plugin filling each role, and RDB$PROCEDURES / RDB$FUNCTIONS record
  the module!entry binding as ordinary metadata.
  See ../../extensibility.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/extensibility && samples/fpc/bin/extensibility
}
program extensibility;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;

function Pad(const s: AnsiString; w: integer): AnsiString;
begin
  Result := s;
  while Length(Result) < w do Result := Result + ' ';
end;

{Print a result set the way isql would: header, dashes, aligned rows.}
procedure PrintTable(const sql: AnsiString);
var R: IResultSet;
    md: IMetaData;
    names: array of AnsiString;
    rows: array of array of AnsiString;
    w: array of integer;
    i, ri, n: integer;
    s, line: AnsiString;
begin
  R := A.OpenCursor(Tr, sql);
  md := R.GetStatement.MetaData;
  n := md.Count;
  SetLength(names, n);
  SetLength(w, n);
  for i := 0 to n - 1 do
  begin
    names[i] := md[i].Name;
    w[i] := Length(names[i]);
  end;
  SetLength(rows, 0);
  while R.FetchNext do
  begin
    ri := Length(rows);
    SetLength(rows, ri + 1);
    SetLength(rows[ri], n);
    for i := 0 to n - 1 do
    begin
      if R[i].IsNull then s := '<null>' else s := TrimRight(R[i].AsString);
      rows[ri][i] := s;
      if Length(s) > w[i] then w[i] := Length(s);
    end;
  end;
  line := '';
  for i := 0 to n - 1 do line := line + Pad(names[i], w[i]) + ' ';
  writeln(TrimRight(line));
  line := '';
  for i := 0 to n - 1 do line := line + StringOfChar('-', w[i]) + ' ';
  writeln(TrimRight(line));
  for ri := 0 to High(rows) do
  begin
    line := '';
    for i := 0 to n - 1 do line := line + Pad(rows[ri][i], w[i]) + ' ';
    writeln(TrimRight(line));
  end;
end;

begin
  A := AttachOrCreate(DbConn('extensibility'));
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);

  { -- 1. Bind SQL names to entry points in the shipped native module. }
  A.ExecuteSQL(Tr,
    'recreate procedure gen_rows (start_n integer not null, '
    + '                             end_n integer not null) '
    + '  returns (n integer not null) '
    + '  external name ''udrcpp_example!gen_rows'' engine udr', []);
  A.ExecuteSQL(Tr,
    'recreate function sum_args (n1 integer, n2 integer, n3 integer) '
    + '  returns integer '
    + '  external name ''udrcpp_example!sum_args'' engine udr', []);
  Tr.CommitRetaining;

  { -- 2. Call them: native C++ running inside the server, plain SQL here. }
  writeln('select n from gen_rows(1, 5):');
  PrintTable('select n from gen_rows(1, 5)');

  writeln;
  writeln('select sum_args(19, 20, 3):  ',
    A.OpenCursorAtStart(Tr,
      'select sum_args(19, 20, 3) from rdb$database')[0].AsString);

  { -- 3. The binding is ordinary metadata... }
  writeln;
  writeln('external routines recorded in the system tables:');
  PrintTable('select trim(rdb$procedure_name) || ''  ->  '' || '
    + '       trim(rdb$entrypoint) || ''  (engine '' || '
    + '       trim(rdb$engine_name) || '')'' '
    + 'from rdb$procedures where rdb$engine_name = ''UDR'' '
    + 'union all '
    + 'select trim(rdb$function_name) || ''  ->  '' || '
    + '       trim(rdb$entrypoint) || ''  (engine '' || '
    + '       trim(rdb$engine_name) || '')'' '
    + 'from rdb$functions where rdb$engine_name = ''UDR''');

  { -- 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG. }
  writeln;
  writeln('plugins filling each role (rdb$config):');
  PrintTable('select rdb$config_name role, rdb$config_value plugin '
    + 'from rdb$config '
    + 'where rdb$config_name in (''Providers'', ''AuthServer'', '
    + '      ''UserManager'', ''WireCryptPlugin'', ''TracePlugin'', '
    + '      ''DefaultProfilerPlugin'') '
    + 'order by rdb$config_id');

  Tr.Commit;
  writeln;
  writeln('done.');
end.
