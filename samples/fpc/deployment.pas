{
  deployment.pas — the deployment scenario in Pascal.

  The fbintf twin of ../cpp/deployment.cpp: a deployment is usually
  inspected from the server's shell, but the engine publishes its own
  view of it to any SQL client, and this program reads it three layers
  deep — MON$DATABASE (the physical facts), RDB$CONFIG (the EFFECTIVE
  configuration, firebird.conf merged with databases.conf), and the
  SYSTEM context variables (engine version, protocol, session facts).
  Everything is plain SQL through IAttachment.OpenCursorAtStart, so the
  whole sample is read-only and safe against the shared employee
  database.  See ../../deployment-and-operations.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/deployment && samples/fpc/bin/deployment
}
program deployment;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;
    conn: AnsiString;

function Pad(const s: AnsiString; w: integer): AnsiString;
begin
  Result := s;
  while Length(Result) < w do Result := Result + ' ';
end;

function QV(const sql: AnsiString): AnsiString;
var R: IResultSet;
begin
  R := A.OpenCursorAtStart(Tr, sql);
  if R.IsEof then Exit('<no rows>');
  if R[0].IsNull then Exit('<null>');
  Result := TrimRight(R[0].AsString);
end;

procedure LineOut(const lbl, sql: AnsiString);
begin
  writeln('  ', Pad(lbl, 22), ' ', QV(sql));
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
  if ParamCount >= 1 then conn := ParamStr(1)
  else conn := 'localhost:employee';
  A := FirebirdAPI.OpenDatabase(conn, DefaultDPB);
  Tr := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], taCommit);

  writeln('== MON$DATABASE: the database as deployed ==');
  LineOut('database file',  'SELECT MON$DATABASE_NAME FROM MON$DATABASE');
  LineOut('ODS version',    'SELECT MON$ODS_MAJOR || ''.'' || MON$ODS_MINOR FROM MON$DATABASE');
  LineOut('page size',      'SELECT MON$PAGE_SIZE FROM MON$DATABASE');
  LineOut('page buffers',   'SELECT MON$PAGE_BUFFERS FROM MON$DATABASE');
  LineOut('sweep interval', 'SELECT MON$SWEEP_INTERVAL FROM MON$DATABASE');
  LineOut('forced writes',  'SELECT MON$FORCED_WRITES FROM MON$DATABASE');
  LineOut('SQL dialect',    'SELECT MON$SQL_DIALECT FROM MON$DATABASE');
  LineOut('crypt state',    'SELECT MON$CRYPT_STATE FROM MON$DATABASE');

  writeln;
  writeln('== RDB$CONFIG: effective configuration (selected of ',
    QV('SELECT COUNT(*) FROM RDB$CONFIG'), ' settings) ==');
  PrintTable('SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_IS_SET '
    + 'FROM RDB$CONFIG '
    + 'WHERE RDB$CONFIG_NAME IN (''ServerMode'', ''DefaultDbCachePages'', '
    + '  ''DatabaseAccess'', ''WireCrypt'', ''MaxParallelWorkers'', ''SecurityDatabase'') '
    + 'ORDER BY RDB$CONFIG_NAME');

  writeln;
  writeln('== settings explicitly set in config files ==');
  PrintTable('SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_SOURCE '
    + 'FROM RDB$CONFIG WHERE RDB$CONFIG_IS_SET ORDER BY RDB$CONFIG_ID');

  writeln;
  writeln('== SYSTEM context: this engine, this session ==');
  LineOut('ENGINE_VERSION',    'SELECT RDB$GET_CONTEXT(''SYSTEM'',''ENGINE_VERSION'') FROM RDB$DATABASE');
  LineOut('DB_NAME',           'SELECT RDB$GET_CONTEXT(''SYSTEM'',''DB_NAME'') FROM RDB$DATABASE');
  LineOut('NETWORK_PROTOCOL',  'SELECT RDB$GET_CONTEXT(''SYSTEM'',''NETWORK_PROTOCOL'') FROM RDB$DATABASE');
  LineOut('WIRE_CRYPT_PLUGIN', 'SELECT RDB$GET_CONTEXT(''SYSTEM'',''WIRE_CRYPT_PLUGIN'') FROM RDB$DATABASE');
  LineOut('CLIENT_ADDRESS',    'SELECT RDB$GET_CONTEXT(''SYSTEM'',''CLIENT_ADDRESS'') FROM RDB$DATABASE');

  Tr.Commit;
  writeln;
  writeln('done.');
end.
