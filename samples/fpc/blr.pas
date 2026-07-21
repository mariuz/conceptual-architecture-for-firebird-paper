{
  blr.pas — stored BLR read raw from the catalog and decoded.

  The fbintf twin of ../cpp/blr.cpp, companion to
  ../../blr-intermediate-language.md.  Reads stored BLR from the stock
  employee database: the computed column EMPLOYEE.FULL_NAME
  (RDB$FIELDS.RDB$COMPUTED_BLR) and the procedure GET_EMP_PROJ
  (RDB$PROCEDURES.RDB$PROCEDURE_BLR) are fetched as blobs — a one-liner
  here, ISQLData.AsBlob.GetAsString into a rawbytestring, where the C++
  twin walks openBlob/getSegment by hand — hex-dumped, then the first
  bytes are decoded with the real opcode values from fbintf's blr.inc,
  the Pascal translation of the same firebird/impl/blr.h the engine
  compiles.  The expression decoder walks the whole prefix-encoded tree;
  the procedure decoder stops after the message declarations (isql's
  SET BLOB ALL renders the rest symbolically).

  Read-only against employee.

  Build & run (see ../README.md):
      make -C samples/fpc bin/blr && samples/fpc/bin/blr
}
program blr;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, Math, IB, FBHandsOn;

type TBlrBytes = array of byte;

var Att: IAttachment;
    Tr: ITransaction;

{Fetch a single-row, single-BLOB-column query and return the blob's bytes.}
function FetchBlob(const sql: AnsiString): TBlrBytes;
var s: rawbytestring;
begin
  Result := nil;
  s := Att.OpenCursorAtStart(Tr, sql)[0].AsBlob.GetAsString;
  SetLength(Result, Length(s));
  if Length(s) > 0 then
    Move(s[1], Result[0], Length(s));
end;

procedure HexDump(const b: TBlrBytes; limit: integer);
var i: integer;
begin
  for i := 0 to Min(High(b), limit - 1) do
    if i mod 16 = 15 then writeln(IntToHex(b[i], 2))
    else write(IntToHex(b[i], 2), ' ');
  if Length(b) > limit then
    writeln('... (', Length(b), ' bytes total)')
  else
    writeln('(', Length(b), ' bytes total)');
end;

{Decode one *expression* — enough opcodes for a computed column.}
procedure Expr(const b: TBlrBytes; var p: integer; depth: integer);
var op, ctx, len: integer;
    cs, slen: integer;
    txt: AnsiString;
begin
  write(StringOfChar(' ', depth * 3));
  op := b[p]; Inc(p);
  case op of
    blr_concatenate:
    begin
      writeln('blr_concatenate');
      Expr(b, p, depth + 1);
      Expr(b, p, depth + 1);
    end;
    blr_field:
    begin
      ctx := b[p]; Inc(p);
      len := b[p]; Inc(p);
      SetString(txt, PAnsiChar(@b[p]), len);
      Inc(p, len);
      writeln('blr_field context ', ctx, ', ''', txt, '''');
    end;
    blr_literal:
      if b[p] = blr_text2 then
      begin
        Inc(p);
        cs := b[p] or (b[p+1] shl 8);
        slen := b[p+2] or (b[p+3] shl 8);
        Inc(p, 4);
        SetString(txt, PAnsiChar(@b[p]), slen);
        Inc(p, slen);
        writeln('blr_literal blr_text2 charset ', cs, ', len ', slen,
          ', "', txt, '"');
      end
      else
      begin
        writeln('blr_literal dtype ', b[p], ' ...');
        p := Length(b);
      end;
    else
    begin
      writeln('opcode ', op, ' (decoder stops here)');
      p := Length(b);
    end;
  end;
end;

var b: TBlrBytes;
    DPB: IDPB;
    p, msg, count, i: integer;
    conn: AnsiString;

begin
  conn := 'localhost:employee';
  if ParamCount >= 1 then conn := ParamStr(1);

  {charset NONE: system BLR blobs are bytes, not text to transliterate}
  DPB := FirebirdAPI.AllocateDPB;
  DPB.Add(isc_dpb_user_name).AsString := HandsOnUser;
  DPB.Add(isc_dpb_password).AsString := HandsOnPassword;
  Att := FirebirdAPI.OpenDatabase(conn, DPB);
  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);

  writeln('== computed column EMPLOYEE.FULL_NAME - RDB$FIELDS.RDB$COMPUTED_BLR');
  b := FetchBlob(
    'SELECT f.RDB$COMPUTED_BLR FROM RDB$FIELDS f' +
    ' JOIN RDB$RELATION_FIELDS rf ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE' +
    ' WHERE rf.RDB$RELATION_NAME = ''EMPLOYEE''' +
    ' AND rf.RDB$FIELD_NAME = ''FULL_NAME''');
  HexDump(b, 64);

  p := 0;
  if b[p] = blr_version5 then writeln('blr_version5')
  else writeln('unexpected version!');
  Inc(p);
  Expr(b, p, 1);
  if (p < Length(b)) and (b[p] = blr_eoc) then writeln('blr_eoc')
  else writeln('(no blr_eoc?)');

  writeln;
  writeln('== procedure GET_EMP_PROJ - RDB$PROCEDURES.RDB$PROCEDURE_BLR');
  b := FetchBlob(
    'SELECT RDB$PROCEDURE_BLR FROM RDB$PROCEDURES' +
    ' WHERE RDB$PROCEDURE_NAME = ''GET_EMP_PROJ''');
  HexDump(b, 32);

  {The opening bytes: version, begin, then the message declarations
   (the wire-format row layouts this doc and the protocol doc share).}
  p := 0;
  if b[p] = blr_version5 then write('blr_version5, ') else write('?, ');
  Inc(p);
  if b[p] = blr_begin then writeln('blr_begin') else writeln('?');
  Inc(p);
  while b[p] = blr_message do
  begin
    Inc(p);
    msg := b[p]; Inc(p);
    count := b[p] or (b[p+1] shl 8); Inc(p, 2);
    write('blr_message ', msg, ', ', count, ' fields:');
    for i := 1 to count do
    begin
      if b[p] = blr_short then
      begin
        write(' blr_short(scale ', b[p+1], ')');
        Inc(p, 2);
      end
      else if b[p] = blr_text2 then
      begin
        write(' blr_text2(cs ', b[p+1] or (b[p+2] shl 8),
          ', len ', b[p+3] or (b[p+4] shl 8), ')');
        Inc(p, 5);
      end
      else
      begin
        write(' dtype ', b[p], '?');
        break;
      end;
    end;
    writeln;
  end;
  writeln('... ', Length(b) - p,
    ' more bytes - see isql SET BLOB ALL for the full dump');

  Tr.Commit;
end.
