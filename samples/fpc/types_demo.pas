{
  types_demo.pas — the standout Firebird types, round-tripped through fbintf.

  The fbintf twin of ../cpp/types.cpp: BOOLEAN, INT128, DECFLOAT(34),
  TIMESTAMP WITH TIME ZONE and a CHECK-constrained domain.  (Named
  types_demo because a program file called types.pas would shadow the
  FPC RTL unit Types that fbintf's uses-closure needs — the same clash
  that made the C++ twin transactions_demo.cpp.)

  Where the OO-API sample reads SQL_* codes out of IMessageMetadata by
  hand, fbintf's IColumnMetaData exposes GetSQLType AND GetSQLTypeName;
  and beyond the engine's text conversion the C++ twin settles for,
  fbintf offers real typed access: INT128 and DECFLOAT(34) arrive as
  TBCD (FmtBCD), BOOLEAN as Pascal boolean, and TIMESTAMP WITH TIME
  ZONE decodes to wall time + dst offset + the zone NAME — none of
  which the JS or Rust twins could reach typed.  The domain CHECK
  violation surfaces as EIBInterBaseError with the gds code.
  See ../../sql-dialect-and-types.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/types_demo && samples/fpc/bin/types_demo
}
program types_demo;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, FmtBCD, IB, FBHandsOn;

const DDL_TPB: array[0..3] of byte = (isc_tpb_write, isc_tpb_nowait,
        isc_tpb_read_committed, isc_tpb_rec_version);

var Att: IAttachment;
    Tr: ITransaction;
    Stmt: IStatement;
    Cur: IResultSet;
    Meta: IMetaData;
    i: integer;
    sql: AnsiString;
    bcd: TBCD;
    dt: TDateTime;
    dstOffset: smallint;
    tzName: AnsiString;
    tzID: TFBTimeZoneID;

begin
  Att := AttachOrCreate(DbConn('types'));

  { Idempotent cleanup.  Each drop commits on its own: DDL is partly
    deferred to commit time (DFW), so DROP DOMAIN's dependency check must
    run after the table drop is actually committed. }
  for sql in ['DROP TABLE showcase', 'DROP DOMAIN d_email'] do
    try
      Att.ExecImmediate(DDL_TPB, sql);
    except on EIBInterBaseError do ;
    end;

  { DDL: a domain plus one table using the FB3/FB4 headline types. }
  Att.ExecImmediate(DDL_TPB,
    'CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE ''%@%'')');
  Att.ExecImmediate(DDL_TPB,
    'CREATE TABLE showcase (' +
    '  flag  BOOLEAN,' +
    '  big   INT128,' +
    '  money DECFLOAT(34),' +
    '  born  TIMESTAMP WITH TIME ZONE,' +
    '  mail  d_email)');

  { One row exercising each type's edge. }
  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  Att.ExecuteSQL(Tr,
    'INSERT INTO showcase VALUES (' +
    '  TRUE,' +
    '  170141183460469231731687303715884105727,' +          { INT128 max }
    '  0.1,' +                                              { exact in DECFLOAT }
    '  TIMESTAMP ''2026-07-21 12:00:00 Europe/Bucharest'',' + { named zone }
    '  ''user@example.com'')', []);

  { The domain's CHECK travels with the type: a mail without '@' dies. }
  try
    Att.ExecuteSQL(Tr, 'INSERT INTO showcase (mail) VALUES (''not-an-address'')', []);
    writeln('BUG: domain CHECK did not fire');
  except
    on E: EIBInterBaseError do
      writeln('domain CHECK rejected ''not-an-address'':'#10'    gds ',
        E.IBErrorCode, ': ', Copy(E.Message, 1, 120), #10);
  end;

  { What the wire carries: type codes straight from the statement metadata —
    fbintf names them for us via GetSQLTypeName. }
  Stmt := Att.Prepare(Tr, 'SELECT * FROM showcase');
  Meta := Stmt.MetaData;
  writeln('column  wire type (IColumnMetaData.GetSQLType)');
  writeln('------  --------------------------------------');
  for i := 0 to Meta.Count - 1 do
    writeln(Format('%-7s %5d = %s',
      [Meta[i].getName, Meta[i].GetSQLType, Meta[i].GetSQLTypeName]));

  { Typed access — the fbintf privilege.  The JS/Rust twins had to take
    strings for INT128/DECFLOAT; fbintf hands over real TBCD values and a
    decoded time zone name. }
  Cur := Stmt.OpenCursor;
  Cur.FetchNext;
  writeln;
  writeln('typed round-trip:');
  writeln('  flag  AsBoolean       : ', Cur.ByName('FLAG').AsBoolean);
  bcd := Cur.ByName('BIG').AsBCD;
  writeln('  big   AsBCD (TBCD)    : ', BCDToStr(bcd));
  bcd := Cur.ByName('MONEY').AsBCD;
  writeln('  money AsBCD (TBCD)    : ', BCDToStr(bcd));
  Cur.ByName('BORN').GetAsDateTime(dt, dstOffset, tzID);
  Cur.ByName('BORN').GetAsDateTime(dt, dstOffset, tzName);
  writeln('  born  GetAsDateTime   : ', FormatDateTime('yyyy-mm-dd hh:nn:ss', dt),
    ' ', tzName, '  (dst offset ', dstOffset, ' min, zone id ', tzID, ')');
  writeln('  mail  AsString        : ', Cur.ByName('MAIL').AsString);

  { And the same row as text.  Note the difference from the C++ twin:
    there the VARCHAR coercion made the SERVER's CVT rules produce the
    strings; fbintf's AsString formats client-side (FBSQLData), so the
    timestamp below follows the client's date format and the default
    tzOffset text option (+03:00 instead of the zone name). }
  writeln;
  writeln('AsString round-trip (fbintf client-side formatting):');
  for i := 0 to Cur.Count - 1 do
    writeln('  ', Format('%-7s', [Cur[i].Name]), ' ', Cur[i].AsString);
  Cur.Close;
  Tr.Commit;

  writeln;
  writeln('done.');
end.
