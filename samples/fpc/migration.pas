{
  migration.pas — the migration-and-interoperability scenario in Pascal.

  The fbintf twin of ../cpp/migration.cpp: a probe table with the
  Firebird types migrations trip over (INT128, DECFLOAT(34),
  NUMERIC(38,8), TIMESTAMP WITH TIME ZONE, BOOLEAN, OCTETS/UUID),
  inspected the two ways a migration tool sees them —

    1. the DESCRIBED metadata: IStatement.GetMetaData exposes the raw
       SQL_* wire type codes AND GetSQLTypeName, so a Pascal driver
       need not maintain its own code->name table;
    2. the TYPED face: where the C++ twin coerces everything to VARCHAR
       (the "select and copy strings" ETL view), fbintf hands each value
       over natively - INT128, NUMERIC(38,8) and DECFLOAT(34) arrive as
       TBCD (FmtBCD), TIMESTAMP WITH TIME ZONE decodes to wall time +
       zone id + zone name, BOOLEAN is a Pascal boolean.  The JS and
       Rust twins declared most of these types out of reach.

  See ../../migration-and-interoperability.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/migration && samples/fpc/bin/migration
}
program migration;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, FmtBCD, IB, FBHandsOn;

var Att: IAttachment;
    Tr: ITransaction;
    Stmt: IStatement;
    MD: IMetaData;
    Col: IColumnMetaData;
    Cur: IResultSet;
    i: integer;
    dt: TDateTime;
    dstOffset: smallint;
    tzID: TFBTimeZoneID;
    tzName: AnsiString;
    uuid: RawByteString;
    hex: AnsiString;

begin
  Att := AttachOrCreate(DbConn('migration'));
  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);

  try
    Att.ExecuteSQL(Tr, 'DROP TABLE TYPE_PROBE', []);
  except on EIBInterBaseError do ;
  end;
  Att.ExecuteSQL(Tr,
    'CREATE TABLE TYPE_PROBE ('
    + '  C_INT128 INT128,'
    + '  C_NUM    NUMERIC(38,8),'          { stored in an INT128 too }
    + '  C_DEC    DECFLOAT(34),'
    + '  C_TSTZ   TIMESTAMP WITH TIME ZONE,'
    + '  C_BOOL   BOOLEAN,'
    + '  C_UUID   CHAR(16) CHARACTER SET OCTETS,'
    + '  C_VC     VARCHAR(20))', []);
  Tr.CommitRetaining;
  Att.ExecuteSQL(Tr,
    'INSERT INTO TYPE_PROBE VALUES ('
    + '  170141183460469231731687303715884105727,'     { max INT128 }
    + '  123456789012345678901234567890.12345678,'
    + '  1.234567890123456789012345678901234E+10,'
    + '  TIMESTAMP ''2026-07-21 12:00:00 Europe/Bucharest'','
    + '  TRUE, GEN_UUID(), ''naïve ütf8 text'')', []);
  Tr.CommitRetaining;

  { -- 1. what DESCRIBE tells a driver: the wire type codes ----------- }
  writeln('described output metadata of SELECT * FROM TYPE_PROBE:');
  writeln;
  writeln(Format('%-8s %6s %-18s %6s %5s', ['column', 'code', 'wire type', 'length', 'scale']));
  Stmt := Att.Prepare(Tr, 'SELECT * FROM TYPE_PROBE');
  MD := Stmt.GetMetaData;
  for i := 0 to MD.getCount - 1 do
  begin
    Col := MD[i];
    writeln(Format('%-8s %6d %-18s %6d %5d',
      [Col.getName, Col.GetSQLType, Col.GetSQLTypeName, Col.GetSize, Col.getScale]));
  end;

  { -- 2. the typed face: what a Pascal migration tool actually gets -- }
  writeln;
  writeln('same row fetched typed (no string coercion needed):');
  writeln;
  Cur := Stmt.OpenCursor;
  Cur.FetchNext;
  writeln('  C_INT128 = ', BCDToStr(Cur.ByName('C_INT128').GetAsBCD),
    '   (TBCD via AsBCD)');
  writeln('  C_NUM    = ', BCDToStr(Cur.ByName('C_NUM').GetAsBCD),
    '   (TBCD; INT128-backed, wire scale ', Cur.ByName('C_NUM').getScale, ')');
  writeln('  C_DEC    = ', Att.OpenCursorAtStart(Tr,
    'SELECT CAST(C_DEC AS VARCHAR(50)) FROM TYPE_PROBE')[0].AsString,
    '   (engine-rendered)');
  writeln('             fbintf 1.4.9 bug: AsBCD decodes this value as');
  writeln('             ', BCDToStr(Cur.ByName('C_DEC').GetAsBCD),
    ' - the decode masks the');
  writeln('             BCD places with $2f instead of $3f (FB30ClientAPI.',
    'SQLDecFloatDecode),');
  writeln('             so 16..31 decimal places lose bit 4 (23 became 7)');
  Cur.ByName('C_TSTZ').GetAsDateTime(dt, dstOffset, tzID);
  Cur.ByName('C_TSTZ').GetAsDateTime(dt, dstOffset, tzName);
  writeln('  C_TSTZ   = ', FormatDateTime('yyyy-mm-dd hh:nn:ss', dt),
    ' ', tzName, '   (zone id ', tzID, ', dst offset ', dstOffset, ' min)');
  writeln('  C_BOOL   = ', Cur.ByName('C_BOOL').AsBoolean,
    '   (Pascal boolean)');
  uuid := Cur.ByName('C_UUID').AsString;    { OCTETS: raw bytes }
  hex := '';
  for i := 1 to Length(uuid) do
    hex := hex + IntToHex(ord(uuid[i]), 2);
  writeln('  C_UUID   = ', hex, '   (16 raw OCTETS bytes)');
  writeln('  C_VC     = ', Cur.ByName('C_VC').AsString);
  Cur.Close;

  { The engine's own text rendering of the UUID, as the C++ twin shows it. }
  writeln('  C_UUID   = ', Att.OpenCursorAtStart(Tr,
    'SELECT UUID_TO_CHAR(C_UUID) FROM TYPE_PROBE')[0].AsString,
    '   (rendered via UUID_TO_CHAR)');

  Tr.Commit;
  writeln;
  writeln('done.');
end.
