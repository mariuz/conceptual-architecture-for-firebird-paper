{
  monitoring.pas — the monitoring-and-tuning scenario in Pascal.

  The fbintf twin of ../cpp/monitoring.cpp: walks the MON$ hierarchy
  (database -> attachment -> transaction -> statement, counters joined
  via MON$STAT_ID) and then demonstrates the defining architectural
  property of the monitoring tables: the first MON$ select in a
  transaction takes a STABLE SNAPSHOT.  A full-scan workload runs, the
  same transaction still sees the old counters, a new transaction sees
  them refreshed.  See ../../monitoring-and-tuning.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/monitoring && samples/fpc/bin/monitoring
}
program monitoring;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const
  { This attachment's record/page counters, via the MON$STAT_ID join. }
  COUNTERS =
    'SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS, R.MON$RECORD_INSERTS, '
    + '     I.MON$PAGE_FETCHES, I.MON$PAGE_READS '
    + 'FROM MON$ATTACHMENTS A '
    + 'JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID '
    + 'JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID '
    + 'WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION';

var Att: IAttachment;
    Tr: ITransaction;

procedure PrintRS(rs: IResultSet);
var i: integer;
    line: AnsiString;
    first: boolean;
begin
  first := true;
  while rs.FetchNext do
  begin
    if first then
    begin
      line := '';
      for i := 0 to rs.getCount - 1 do
      begin
        if i > 0 then line := line + ' | ';
        line := line + rs[i].Name;
      end;
      writeln(line);
      first := false;
    end;
    line := '';
    for i := 0 to rs.getCount - 1 do
    begin
      if i > 0 then line := line + ' | ';
      line := line + Trim(rs[i].AsString);
    end;
    writeln(line);
  end;
  rs.Close;
end;

function Q(const sql: AnsiString): AnsiString;
begin
  Result := Att.OpenCursorAtStart(Tr, sql)[0].AsString;
end;

begin
  Att := AttachOrCreate(DbConn('monitoring'));
  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);

  { Workload table: 10000 rows to scan (idempotent). }
  try
    Att.ExecuteSQL(Tr, 'DROP TABLE MON_WORK', []);
  except on EIBInterBaseError do ;
  end;
  Att.ExecuteSQL(Tr, 'CREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)', []);
  Tr.CommitRetaining;
  Att.ExecuteSQL(Tr,
    'EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN '
    + '  WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); I = I + 1; END '
    + 'END', []);
  Tr.Commit;

  { -- 1. the hierarchy, one level per query, one consistent snapshot -- }
  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('== MON$DATABASE: transaction markers ==');
  PrintRS(Att.OpenCursor(Tr,
    'SELECT MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$NEXT_TRANSACTION, '
    + '     MON$PAGE_BUFFERS FROM MON$DATABASE'));

  writeln;
  writeln('== MON$ATTACHMENTS -> MON$TRANSACTIONS -> MON$STATEMENTS (me) ==');
  PrintRS(Att.OpenCursor(Tr,
    'SELECT A.MON$ATTACHMENT_ID, A.MON$USER, T.MON$TRANSACTION_ID, '
    + '     S.MON$STATE, CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 40) AS VARCHAR(40)) AS SQL_HEAD '
    + 'FROM MON$ATTACHMENTS A '
    + 'JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID '
    + 'JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID '
    + 'WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION'));

  { -- 2. the snapshot property, measured on our own counters -- }
  writeln;
  writeln('== my counters (snapshot 1) ==');
  PrintRS(Att.OpenCursor(Tr, COUNTERS));

  writeln;
  writeln('... running workload: SELECT COUNT(*) full scan + indexed lookup ...');
  writeln('count = ', Q('SELECT COUNT(*) FROM MON_WORK'),
    ', point = ', Q('SELECT VAL FROM MON_WORK WHERE ID = 4242'));

  writeln;
  writeln('== same transaction, re-queried: STILL snapshot 1 ==');
  PrintRS(Att.OpenCursor(Tr, COUNTERS));
  Tr.Commit;

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln;
  writeln('== new transaction: fresh snapshot, workload now visible ==');
  PrintRS(Att.OpenCursor(Tr, COUNTERS));
  Tr.Commit;

  writeln;
  writeln('done.');
end.
