{
  catalog.pas — the catalog describing itself, from client SQL.

  The fbintf twin of ../cpp/catalog.cpp, companion to
  ../../catalog-bootstrap.md.  On a freshly created database it shows:
    1. the fixed relation ids burned into relations.h declaration order
       (RDB$PAGES 0, RDB$DATABASE 1, RDB$FIELDS 2, RDB$RELATIONS 6);
    2. RDB$PAGES carrying its own pointer page — and the hdr_PAGES word
       on page 0 agreeing with it (the anti-recursion anchor);
    3. RDB$FORMATS empty while sixty-odd system relations with hundreds
       of columns are fully usable: their formats are compiled into
       libEngine (INI_init), not stored;
    4. a user table gaining RDB$FORMATS rows the moment DDL creates and
       alters it — user formats live in the catalog.

  Build & run (see ../README.md):
      make -C samples/fpc bin/catalog && samples/fpc/bin/catalog
}
program catalog;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, BaseUnix, IB, FBHandsOn;

{Print a result set as a left-aligned table with the given headers.}
procedure PrintTable(Att: IAttachment; Tr: ITransaction; const sql: AnsiString;
  const headers: array of AnsiString);
var R: IResultset;
    rows: array of array of AnsiString;
    w: array of integer;
    i, n: integer;
    line: AnsiString;
begin
  R := Att.OpenCursor(Tr, sql);
  SetLength(w, Length(headers));
  for i := 0 to High(headers) do
    w[i] := Length(headers[i]);
  n := 0;
  while R.FetchNext do
  begin
    SetLength(rows, n + 1);
    SetLength(rows[n], R.Count);
    for i := 0 to R.Count - 1 do
    begin
      if R[i].IsNull then rows[n][i] := '<null>'
      else rows[n][i] := trim(R[i].AsString);
      if Length(rows[n][i]) > w[i] then w[i] := Length(rows[n][i]);
    end;
    Inc(n);
  end;
  line := '';
  for i := 0 to High(headers) do
    line := line + Format('%-*s  ', [w[i], headers[i]]);
  writeln(TrimRight(line));
  for n := 0 to High(rows) do
  begin
    line := '';
    for i := 0 to High(rows[n]) do
      line := line + Format('%-*s  ', [w[i], rows[n][i]]);
    writeln(TrimRight(line));
  end;
end;

var Att: IAttachment;
    Tr: ITransaction;
    fd: cint;
    hdrPages: longword;
    conn, localFile: AnsiString;

begin
  conn := DbConn('catalog');
  localFile := DbPath('catalog');

  {A truly fresh database each run: drop it if it exists, recreate.}
  Att := FirebirdAPI.OpenDatabase(conn, DefaultDPB, false);
  if Att <> nil then
    Att.DropDatabase;
  Att := FirebirdAPI.CreateDatabase(conn, DefaultDPB);

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);

  writeln('-- 1. fixed relation ids (relations.h declaration order) --');
  PrintTable(Att, Tr,
    'select rdb$relation_id, trim(rdb$relation_name) ' +
    'from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1',
    ['ID', 'NAME']);

  writeln;
  writeln('-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --');
  PrintTable(Att, Tr,
    'select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type ' +
    'from rdb$pages where rdb$relation_id in (0, 6) ' +
    'order by rdb$relation_id, rdb$page_type, rdb$page_number',
    ['RDB$PAGE_NUMBER', 'RDB$RELATION_ID', 'RDB$PAGE_SEQUENCE', 'RDB$PAGE_TYPE']);

  {The anchor that cuts the recursion: hdr_PAGES at byte 28 of page 0.
   Plain FpOpen/FpRead: the server holds advisory locks on the live file,
   which TFileStream's share modes would trip over.}
  fd := FpOpen(localFile, O_RDONLY);
  if fd >= 0 then
  begin
    hdrPages := 0;
    if (FpLseek(fd, 28, SEEK_SET) = 28) and
       (FpRead(fd, hdrPages, 4) = 4) then   {little-endian ULONG on this platform}
    begin
      writeln;
      writeln('hdr_PAGES (page 0, offset 28) = ', hdrPages,
        '  <- matches the (relation 0, type 4) row above');
    end;
    FpClose(fd);
  end
  else
    writeln('could not open ', localFile, ' for the page-0 peek');

  writeln;
  writeln('-- 3. formats as code: zero stored formats, yet a full catalog --');
  PrintTable(Att, Tr,
    'select (select count(*) from rdb$formats), ' +
    '       (select count(*) from rdb$relations where rdb$system_flag = 1), ' +
    '       (select count(*) from rdb$relation_fields r join rdb$relations rel ' +
    '          on r.rdb$relation_name = rel.rdb$relation_name ' +
    '          and r.rdb$schema_name = rel.rdb$schema_name ' +
    '        where rel.rdb$system_flag = 1) ' +
    'from rdb$database',
    ['FORMATS_ROWS', 'SYS_RELATIONS', 'SYS_FIELDS']);
  Tr.Commit;

  writeln;
  writeln('-- 4. user DDL writes formats into the catalog --');
  Att.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'create table t1 (a integer)');
  Att.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'alter table t1 add b varchar(10)');

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);
  PrintTable(Att, Tr,
    'select rdb$relation_id, rdb$format, octet_length(rdb$descriptor) ' +
    'from rdb$formats order by rdb$relation_id, rdb$format',
    ['RDB$RELATION_ID', 'RDB$FORMAT', 'DESCRIPTOR_BYTES']);
  writeln;
  writeln('(relation id of T1: ', Att.OpenCursorAtStart(Tr,
      'select rdb$relation_id from rdb$relations ' +
      'where rdb$relation_name = ''T1''')[0].AsString,
    ' - the first user id; system tables still contribute no rows)');
  Tr.Commit;

  writeln('done.');
end.
