{
  temporal.pas — TIMESTAMP WITH TIME ZONE's two faces, via fbintf.

  The fbintf twin of ../cpp/temporal.cpp: the same storage-model tour.
  The OO-API sample memcpy'd ISC_TIMESTAMP_TZ out of the message buffer
  and called IUtil::decodeTimeStampTz by hand; fbintf exposes both faces
  as typed getters on the same ISQLData — GetAsDateTime(dt, dstOffset,
  tzID) yields the 2-byte wire zone id, GetAsDateTime(dt, dstOffset,
  tzName) the decoded zone NAME, and GetAsUTCDateTime the UTC instant
  the engine actually stores (from which the wire's day/time fields are
  reconstructed below).  ITimeZoneServices (FB30TimeZoneServices under
  the hood) is the client-side window on the same tzdata the engine
  uses: TimeZoneID2TimeZoneName, GetEffectiveOffsetMins.  The
  DST-boundary, instant-equality and SET TIME ZONE demonstrations are
  pure SQL and port unchanged.  See ../../temporal-and-time-zones.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/temporal && samples/fpc/bin/temporal
}
program temporal;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var Att: IAttachment;
    Tr: ITransaction;
    Cur: IResultSet;
    TZS: ITimeZoneServices;
    i: integer;
    dt, utc: TDateTime;
    dstOffset: smallint;
    tzID: TFBTimeZoneID;
    tzName: AnsiString;
    days, timeUnits: int64;

function One(sql: AnsiString): AnsiString;
begin
  Result := Att.OpenCursorAtStart(Tr, sql)[0].AsString;
end;

begin
  Att := AttachOrCreate(DbConn('temporal'));
  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  TZS := Att.GetTimeZoneServices;

  { -- 1./2. Both faces of two literals: named zone vs bare offset. }
  Cur := Att.OpenCursorAtStart(Tr,
    'SELECT TIMESTAMP ''2026-07-18 12:00:00 America/New_York'',' +
    '       TIMESTAMP ''2026-07-18 12:00:00 -05:00'' FROM RDB$DATABASE');
  for i := 0 to 1 do
  begin
    Cur[i].GetAsDateTime(dt, dstOffset, tzID);   { wall time + wire zone id }
    Cur[i].GetAsDateTime(dt, dstOffset, tzName); { same, zone id -> NAME }
    utc := Cur[i].GetAsUTCDateTime;              { the stored UTC instant }
    { Reconstruct the ISC_TIMESTAMP_TZ day/time fields the wire carries:
      days since 1858-11-17 (Modified JD), time in 100-microsecond units. }
    days := Trunc(utc) + 15018;                  { TDateTime epoch 1899-12-30 }
    timeUnits := Round(Frac(utc) * 24 * 60 * 60 * 10000);
    if i = 0 then writeln('named-zone literal:')
             else writeln('offset literal:');
    writeln('  on the wire : UTC days=', days, ' time=', timeUnits,
      '  zone id=', tzID);
    writeln('  decoded     : ', FormatDateTime('yyyy-mm-dd hh:nn:ss', dt),
      ' ', tzName);
    writeln('  zone id -> name via ITimeZoneServices: ',
      TZS.TimeZoneID2TimeZoneName(tzID),
      '  (effective offset ', TZS.GetEffectiveOffsetMins(dt, tzID), ' min)');
  end;
  { The instant moved to 16:00 UTC; the zone survived: Firebird remembers
    where the value was, not just when. }

  { -- 3. The same wall time across a DST boundary: New York noon is
          17:00 UTC in winter (EST) but 16:00 UTC in summer (EDT). }
  writeln;
  writeln('NY 12:00 in UTC, winter: ', One(
    'SELECT TIMESTAMP ''2026-01-18 12:00:00 America/New_York''' +
    ' AT TIME ZONE ''Etc/UTC'' FROM RDB$DATABASE'));
  writeln('NY 12:00 in UTC, summer: ', One(
    'SELECT TIMESTAMP ''2026-07-18 12:00:00 America/New_York''' +
    ' AT TIME ZONE ''Etc/UTC'' FROM RDB$DATABASE'));

  { Equality is by UTC instant, regardless of zone spelling. }
  writeln('10:00 -02:00 = 09:00 -03:00 ? ', Trim(One(
    'SELECT IIF(TIME ''10:00:00 -02:00'' = TIME ''09:00:00 -03:00'',' +
    ' ''EQUAL'', ''different'') FROM RDB$DATABASE')));

  { -- 4. The session time zone governs "now" and zoneless conversions. }
  writeln;
  writeln('session zone: ', One(
    'SELECT RDB$GET_CONTEXT(''SYSTEM'',''SESSION_TIMEZONE'') FROM RDB$DATABASE'),
    '   CURRENT_TIMESTAMP: ', One(
    'SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE'));
  Att.ExecImmediate(Tr, 'SET TIME ZONE ''Asia/Tokyo''');
  writeln('session zone: ', One(
    'SELECT RDB$GET_CONTEXT(''SYSTEM'',''SESSION_TIMEZONE'') FROM RDB$DATABASE'),
    '     CURRENT_TIMESTAMP: ', One(
    'SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE'));

  Tr.Commit;
  writeln;
  writeln('done.');
end.
