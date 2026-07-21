{
  blobs.pas — the segmented BLOB API end to end, at fbintf altitude.

  The fbintf twin of ../cpp/blobs.cpp, companion to ../../blob-handling.md.
  Create a text blob with three explicit IBlob.Write calls (each call <= 64KB
  is exactly one putSegment underneath), store it through a parameterized
  INSERT (the row holds only the 8-byte blob id), read it back, and ask the
  blob itself for its statistics with IBlob.GetInfo — segment count, longest
  segment, total length, segmented vs stream type.  Then the catalog view of
  subtype/charset for a TEXT vs BINARY column, and BLOB_APPEND assembling a
  blob in SQL.

  Honesty notes against the C++ twin: fbintf's IBlob.Read is stream-style —
  it coalesces segments into the caller's buffer instead of stopping at
  segment boundaries the way raw getSegment does (GetInfo still proves the
  three segments exist on disk).  And IB.pas declares no Seek method at all:
  stream-blob seeking sits below fbintf's veneer.

  Build & run (see ../README.md):
      make -C samples/fpc bin/blobs && samples/fpc/bin/blobs
}
program blobs;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const
  Segments: array[0..2] of AnsiString =
    ('first segment', 'second, longer segment', 'third');

var Att: IAttachment;
    Tr: ITransaction;
    Blob: IBlob;
    Stmt: IStatement;
    R: IResultset;
    NumSegments, MaxSegmentSize, TotalSize: Int64;
    BlobType: TBlobType;
    buf: array[0..63] of AnsiChar;
    chunk: AnsiString;
    i, n: integer;

begin
  Att := AttachOrCreate(DbConn('blobs'));
  Att.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version],
    'recreate table docs (' +
    ' id integer primary key,' +
    ' note blob sub_type text character set utf8,' +
    ' data blob sub_type binary)');

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);

  { -- 1. create a blob with three explicit segments ------------------ }
  Blob := Att.CreateBlob(Tr, 'DOCS', 'NOTE');
  for i := 0 to High(Segments) do
    Blob.Write(Segments[i][1], Length(Segments[i]));
  writeln('wrote 3 segments into a new text blob (one putSegment per IBlob.Write)');

  { Store it: the row itself receives only the 8-byte blob id. }
  Stmt := Att.Prepare(Tr, 'insert into docs (id, note) values (?, ?)');
  Stmt.SQLParams[0].AsInteger := 1;
  Stmt.SQLParams[1].AsBlob := Blob;      {closes the blob, binds its quad id}
  Stmt.Execute;

  { -- 2. read it back: IBlob.Read coalesces the segments ------------- }
  R := Att.OpenCursorAtStart(Tr, 'select note from docs where id = 1');
  Blob := R[0].AsBlob;
  n := 0;
  repeat
    i := Blob.Read(buf, sizeof(buf));
    if i > 0 then
    begin
      Inc(n);
      SetString(chunk, buf, i);
      writeln('  IBlob.Read #', n, ': ', i:2, ' bytes  "', chunk, '"');
    end;
  until i <= 0;
  writeln('(one Read call swallowed all three segments: fbintf streams across');
  writeln(' getSegment boundaries; the raw OO API shows them one by one)');

  { -- 3. the blob describes itself: GetInfo -------------------------- }
  Blob.GetInfo(NumSegments, MaxSegmentSize, TotalSize, BlobType);
  writeln('blob info: ', NumSegments, ' segments, longest ', MaxSegmentSize,
    ', total ', TotalSize, ' bytes, type ', integer(BlobType),
    ' (0=segmented)');

  { -- 4. subtype text vs binary, from the catalog -------------------- }
  writeln;
  writeln('-- column subtypes (RDB$FIELDS) --');
  R := Att.OpenCursor(Tr,
    'select trim(rf.rdb$field_name), f.rdb$field_sub_type, ' +
    '       trim(cs.rdb$character_set_name) ' +
    'from rdb$relation_fields rf ' +
    'join rdb$fields f on rf.rdb$field_source = f.rdb$field_name ' +
    'left join rdb$character_sets cs ' +
    '  on f.rdb$character_set_id = cs.rdb$character_set_id ' +
    'where rf.rdb$relation_name = ''DOCS'' and f.rdb$field_type = 261 ' +
    'order by 1');
  writeln('FIELD  SUBTYPE  CHARSET');
  while R.FetchNext do
  begin
    if R[2].IsNull then chunk := '<null>' else chunk := R[2].AsString;
    writeln(Format('%-5s  %-7s  %s', [R[0].AsString, R[1].AsString, chunk]));
  end;

  { -- 5. BLOB_APPEND: build a blob in SQL without recopying ---------- }
  Att.ExecuteSQL(Tr, 'insert into docs (id, note) values (2, ' +
    'blob_append(cast('''' as blob sub_type text), ' +
    '''part1-'', ''part2-'', ''part3''))', []);
  writeln;
  writeln('-- BLOB_APPEND result --');
  R := Att.OpenCursorAtStart(Tr,
    'select id, octet_length(note), char_length(note), note ' +
    'from docs where id = 2');
  writeln('ID  OCTETS  CHARS  CONTENT');
  writeln(Format('%-2s  %-6s  %-5s  %s',
    [R[0].AsString, R[1].AsString, R[2].AsString,
     R[3].AsString]));   {ISQLData.AsString reads the whole blob - no cast needed}

  Tr.Commit;
  writeln('done.');
end.
