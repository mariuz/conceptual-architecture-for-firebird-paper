{
  intl.pas — the internationalization scenario in Pascal.

  The fbintf twin of ../cpp/intl.cpp: collation decides equality
  ('Café'/'CAFE'/'cafe' match 'cafe' 3x under UNICODE_CI_AI, 1x under
  UCS_BASIC), per-column charsets (UTF8 next to WIN1252 in one table),
  and transliteration driven by the CONNECTION charset: the program
  attaches twice - once with lc_ctype=UTF8 (the FBHandsOn default) and
  once with lc_ctype=NONE via a custom DPB - reads the same stored
  WIN1252 'Café', and hex-dumps the bytes that arrive.  fbintf tags
  every AnsiString with the codepage of the DESCRIBED column charset;
  assigning to a RawByteString exposes the wire bytes untouched.
  See ../../internationalization.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/intl && samples/fpc/bin/intl
}
program intl;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const
  Names: array[0..2] of AnsiString = ('Café', 'CAFE', 'cafe');
  { Non-ASCII text must live in typed AnsiString constants: FPC transcodes
    untyped writeln literals to the (single byte) system default codepage. }
  SQL_UPPER: AnsiString = 'SELECT UPPER(''café èñ ß'') FROM RDB$DATABASE';
  LBL_UPPER: AnsiString = 'UPPER(''café èñ ß'')                      : ';
  SQL_WIN: AnsiString = 'SELECT name_win FROM t WHERE name_bin = ''Café''';
  HDR_WIN: AnsiString = 'SELECT name_win FROM t WHERE name_bin = ''Café'' - same row, two connections:';

var Utf8Att, NoneAtt: IAttachment;
    DPBNone: IDPB;
    Tr: ITransaction;
    RS: IResultSet;
    i: integer;
    line, cs: AnsiString;

function Q(att: IAttachment; tr: ITransaction; const sql: AnsiString): AnsiString;
begin
  Result := att.OpenCursorAtStart(tr, sql)[0].AsString;
end;

procedure HexDump(const lbl: AnsiString; const v: RawByteString);
var i: integer;
    line: AnsiString;
begin
  line := Format('  %-16s len=%2d  ', [lbl, Length(v)]);
  for i := 1 to Length(v) do
    line := line + IntToHex(ord(v[i]), 2) + ' ';
  writeln(line, ' "', v, '"');
end;

{Fetch one value keeping the wire bytes: RawByteString skips any codepage
 conversion FPC would apply to a plain AnsiString assignment.  csName
 returns the charset the column was DESCRIBED with on this connection.}
function RawFetch(att: IAttachment; tr: ITransaction;
  const sql: AnsiString; out csName: AnsiString): RawByteString;
var rs: IResultSet;
begin
  rs := att.OpenCursorAtStart(tr, sql);
  Result := rs[0].AsString;
  csName := att.GetCharsetName(rs[0].getCharSetID);
end;

begin
  Utf8Att := AttachOrCreate(DbConn('intl'));   { connection charset UTF8 }

  { Scratch table (idempotent). }
  try
    Utf8Att.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
      isc_tpb_rec_version], 'DROP TABLE t');
  except on EIBInterBaseError do ;
  end;
  Utf8Att.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version],
    'CREATE TABLE t ('
    + '  name_ci_ai VARCHAR(30) CHARACTER SET UTF8 COLLATE UNICODE_CI_AI,'
    + '  name_bin   VARCHAR(30) CHARACTER SET UTF8 COLLATE UCS_BASIC,'
    + '  name_win   VARCHAR(30) CHARACTER SET WIN1252)');

  Tr := Utf8Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  for i := 0 to High(Names) do
    Utf8Att.ExecuteSQL(Tr, 'INSERT INTO t VALUES (''' + Names[i] + ''','''
      + Names[i] + ''',''' + Names[i] + ''')', []);

  { -- 1. The collation, not the data, decides what "equal" means. }
  writeln('rows matching ''cafe'' with UNICODE_CI_AI : ',
    Q(Utf8Att, Tr, 'SELECT COUNT(*) FROM t WHERE name_ci_ai = ''cafe'''));
  writeln('rows matching ''cafe'' with UCS_BASIC     : ',
    Q(Utf8Att, Tr, 'SELECT COUNT(*) FROM t WHERE name_bin = ''cafe'''));
  writeln(LBL_UPPER, Q(Utf8Att, Tr, SQL_UPPER));
  writeln;

  { Sorting differs too: CI_AI groups the spellings, UCS_BASIC is binary. }
  line := 'ORDER BY name_ci_ai: ';
  RS := Utf8Att.OpenCursor(Tr, 'SELECT name_ci_ai FROM t ORDER BY name_ci_ai');
  while RS.FetchNext do line := line + RS[0].AsString + '  ';
  writeln(line);
  RS.Close;
  line := 'ORDER BY name_bin  : ';
  RS := Utf8Att.OpenCursor(Tr, 'SELECT name_bin FROM t ORDER BY name_bin');
  while RS.FetchNext do line := line + RS[0].AsString + '  ';
  writeln(line, '   (binary: uppercase codepoints first)');
  RS.Close;
  Tr.Commit;
  writeln;

  { -- 2./3. Same stored WIN1252 'Café', two connection charsets.  The
       second attachment builds its own DPB with lc_ctype = NONE. }
  DPBNone := FirebirdAPI.AllocateDPB;
  DPBNone.Add(isc_dpb_user_name).setAsString(HandsOnUser);
  DPBNone.Add(isc_dpb_password).setAsString(HandsOnPassword);
  DPBNone.Add(isc_dpb_lc_ctype).setAsString('NONE');
  NoneAtt := FirebirdAPI.OpenDatabase(DbConn('intl'), DPBNone);

  writeln(HDR_WIN);
  Tr := Utf8Att.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  HexDump('lc_ctype=UTF8:', RawFetch(Utf8Att, Tr, SQL_WIN, cs));
  writeln('     (column described to this connection as charset ', cs, ')');
  Tr.Commit;

  Tr := NoneAtt.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  HexDump('lc_ctype=NONE:', RawFetch(NoneAtt, Tr, SQL_WIN, cs));
  writeln('     (column described to this connection as charset ', cs, ')');
  Tr.Commit;
  writeln('  -> the column stores E9 (WIN1252); the UTF8 connection receives the');
  writeln('     transliterated C3 A9, the NONE connection the raw stored byte.');

  writeln;
  writeln('done.');
end.
