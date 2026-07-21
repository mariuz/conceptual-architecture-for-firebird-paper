{
  request_lifecycle.pas — one CREATE TABLE round trip, instrumented.

  The fbintf twin of ../cpp/request_lifecycle.cpp: the document's exact
  scenario timed phase by phase.  fbintf's IStatement gives the same real
  phase separation as the OO API — Prepare and Execute are distinct calls,
  not a bundled "query" — and around them the engine's own monitoring
  counters (MON$IO_STATS / MON$RECORD_STATS for this attachment) are
  sampled, so the stages of the trace become visible as numbers:

    Prepare              -> DSQL: parser picks DsqlDdlStatement (type DDL)
    Execute              -> EXE/MET: STORE into RDB$RELATIONS etc. — the
                            record-insert counters jump (VIO_store), and the
                            new row is already visible to *this* transaction
    Commit               -> TRA_commit -> DFW_perform_work -> CCH_flush ->
                            PIO_write — the page-write counter jumps

  See ../../request-lifecycle-code-trace.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/request_lifecycle && samples/fpc/bin/request_lifecycle
}
program request_lifecycle;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, BaseUnix, Unix, IB, FBHandsOn;

type TStats = record fetches, marks, writes, recIns: Int64; end;

var A: IAttachment;
    Tr: ITransaction;
    Stmt: IStatement;
    s0, s1, s2: TStats;
    t0: Int64;
    tPrepare, tExecute, tCommit: double;
    typ: AnsiString;

function NowUS: Int64;
var tv: TTimeVal;
begin
  fpGetTimeOfDay(@tv, nil);
  Result := Int64(tv.tv_sec) * 1000000 + tv.tv_usec;
end;

{Sample this attachment's cumulative counters in a fresh transaction
 (MON$ snapshots are frozen per transaction, so a new one sees fresh data).}
function Sample: TStats;
var t: ITransaction;
    R: IResultSet;
begin
  t := A.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);
  R := A.OpenCursorAtStart(t,
    'SELECT i.MON$PAGE_FETCHES, i.MON$PAGE_MARKS, i.MON$PAGE_WRITES,' +
    '       r.MON$RECORD_INSERTS' +
    ' FROM MON$ATTACHMENTS a' +
    ' JOIN MON$IO_STATS i ON a.MON$STAT_ID = i.MON$STAT_ID' +
    ' JOIN MON$RECORD_STATS r ON a.MON$STAT_ID = r.MON$STAT_ID' +
    ' WHERE a.MON$ATTACHMENT_ID = CURRENT_CONNECTION');
  Result.fetches := R[0].AsInt64;
  Result.marks   := R[1].AsInt64;
  Result.writes  := R[2].AsInt64;
  Result.recIns  := R[3].AsInt64;
  t.Commit;
end;

begin
  A := AttachOrCreate(DbConn('request_lifecycle'));

  { Idempotency: drop a leftover table from a previous run, if any. }
  try
    A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
      isc_tpb_rec_version], 'DROP TABLE trace_demo');
  except on EIBInterBaseError do ;
  end;

  s0 := Sample;
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);

  { -- Prepare: Y-valve -> remote -> DSQL (Stages 1-5) ------------------- }
  t0 := NowUS;
  Stmt := A.Prepare(Tr,
    'CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY, name VARCHAR(30))');
  tPrepare := (NowUS - t0) / 1000.0;
  if Stmt.GetSQLStatementType = SQLDDL then typ := 'DDL' else typ := '?';
  writeln(Format('prepare  %6.2f ms   statement type = %s', [tPrepare, typ]));

  { -- Execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) -------- }
  t0 := NowUS;
  Stmt.Execute;
  tExecute := (NowUS - t0) / 1000.0;
  s1 := Sample;
  writeln(Format('execute  %6.2f ms   catalog record inserts: +%d, page marks: +%d',
    [tExecute, s1.recIns - s0.recIns, s1.marks - s0.marks]));

  { Uncommitted, but the STORE into RDB$RELATIONS is visible to the
    transaction that did it: }
  writeln('         in this tx:  RDB$RELATIONS has TRACE_DEMO = ',
    A.OpenCursorAtStart(Tr, 'SELECT COUNT(*) FROM RDB$RELATIONS' +
      ' WHERE RDB$RELATION_NAME = ''TRACE_DEMO''')[0].AsInteger);

  { -- Commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9) ----- }
  t0 := NowUS;
  Tr.Commit;
  tCommit := (NowUS - t0) / 1000.0;
  s2 := Sample;
  writeln(Format('commit   %6.2f ms   page writes: +%d  (fetches: +%d over the whole trip)',
    [tCommit, s2.writes - s1.writes, s2.fetches - s0.fetches]));

  writeln('done.');
end.
