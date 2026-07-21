{
  numerics.pas — the numeric-and-precision-arithmetic scenario in Pascal.

  The fbintf twin of ../cpp/numerics.cpp: the DOUBLE vs DECFLOAT residue
  of (0.1+0.2)-0.3, NUMERIC(18,4) as a scaled integer, INT128 at its
  maximum, and the DECFLOAT division-by-zero trap.  Where the C++ twin
  reads the scaled integer out of the raw message buffer, fbintf hands
  it over typed: IStatement.GetMetaData reports SQL_INT64 with scale -4,
  and ISQLData.GetAsNumeric returns an IFBNumeric whose getRawValue is
  the untouched wire integer.  INT128 and DECFLOAT arrive as TBCD —
  typed FB4 support the node and rust twins had to work around.
  See ../../numeric-and-precision-arithmetic.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/numerics && samples/fpc/bin/numerics
}
program numerics;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, FmtBCD, IB, FBHandsOn;

var Att: IAttachment;
    Tr: ITransaction;
    Stmt: IStatement;
    Col: IColumnMetaData;
    Cur: IResultSet;
    Num: IFBNumeric;

function Q(const sql: AnsiString): AnsiString;
begin
  Result := Att.OpenCursorAtStart(Tr, sql)[0].AsString;
end;

{One line of the (multi-line) engine error message, newlines flattened.}
function Flat(const s: AnsiString): AnsiString;
begin
  Result := StringReplace(Trim(s), #13, '', [rfReplaceAll]);
  Result := StringReplace(Result, #10, ' / ', [rfReplaceAll]);
  if Length(Result) > 120 then Result := Copy(Result, 1, 120) + '...';
end;

begin
  Att := AttachOrCreate(DbConn('numerics'));
  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);

  { -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3. }
  writeln('(0.1+0.2)-0.3 in DOUBLE PRECISION : ',
    Q('SELECT (CAST(0.1 AS DOUBLE PRECISION) + 0.2) - 0.3 FROM RDB$DATABASE'));
  writeln('(0.1+0.2)-0.3 in DECFLOAT(34)     : ',
    Q('SELECT (CAST(0.1 AS DECFLOAT(34)) + 0.2) - 0.3 FROM RDB$DATABASE'));
  writeln;

  { -- 2. NUMERIC(18,4) on the wire: scaled integer + scale in the metadata.
         fbintf describes the column exactly as the raw API does, then
         hands the untouched scaled integer over via IFBNumeric. }
  Stmt := Att.Prepare(Tr,
    'SELECT CAST(12345.6789 AS NUMERIC(18,4)) FROM RDB$DATABASE');
  Col := Stmt.GetMetaData[0];
  writeln('NUMERIC(18,4) wire format: type=', Col.GetSQLType, ' (',
    Col.GetSQLTypeName, '), length=', Col.GetSize, ', scale=', Col.getScale);

  Cur := Stmt.OpenCursor;
  Cur.FetchNext;
  Num := Cur[0].GetAsNumeric;
  writeln('IFBNumeric.getRawValue         : ', Num.getRawValue,
    '   <- the wire integer, untouched');
  writeln('IFBNumeric.getScale            : ', Num.getScale);
  writeln('value = raw * 10^scale         : ', Num.getRawValue, ' * 10^',
    Num.getScale, ' = ', Num.getAsString);
  writeln('ISQLData.AsInt64 (rescaled)    : ', Cur[0].AsInt64,
    '   <- getAsInt64 rescales to integer');
  Cur.Close;
  writeln;

  { -- 3. INT128: the full range, and one step past it.  The value arrives
         as a TBCD - typed INT128 support. }
  Cur := Att.OpenCursorAtStart(Tr,
    'SELECT CAST(170141183460469231731687303715884105727 AS INT128) FROM RDB$DATABASE');
  writeln('INT128 max  : ', BCDToStr(Cur[0].GetAsBCD), '   (via ISQLData.AsBCD)');
  try
    Q('SELECT CAST(170141183460469231731687303715884105727 AS INT128) + 1 FROM RDB$DATABASE');
    writeln('BUG: overflow not detected');
  except on E: EIBInterBaseError do
    writeln('INT128 max+1: gds ', E.IBErrorCode, ': ', Flat(E.Message));
  end;
  writeln;

  { -- 4. DECFLOAT division by zero: trapped by default, Infinity if untrapped. }
  try
    Q('SELECT CAST(1 AS DECFLOAT(16)) / 0 FROM RDB$DATABASE');
    writeln('BUG: default trap did not fire');
  except on E: EIBInterBaseError do
    writeln('1/0 with default traps : gds ', E.IBErrorCode, ': ',
      Flat(E.Message));
  end;
  Att.ExecImmediate(Tr, 'SET DECFLOAT TRAPS TO');   { clear all traps }
  { fbintf gap: it decodes DECFLOAT into TBCD, which has no representation
    for the special values - Infinity arrives as 0.  Let the engine render
    the string instead. }
  writeln('1/0 with traps cleared : ', Q(
    'SELECT CAST(CAST(1 AS DECFLOAT(16)) / 0 AS VARCHAR(20)) FROM RDB$DATABASE'),
    '   (engine-rendered: TBCD cannot hold Infinity)');

  Tr.Commit;
  writeln;
  writeln('done.');
end.
