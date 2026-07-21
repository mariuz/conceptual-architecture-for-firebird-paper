{
  gc_sweep.pas — watching the record collectors work, in Pascal.

  The fbintf twin of ../cpp/gc_sweep.cpp: creates record versions under
  a pinned SNAPSHOT, then releases the snapshot and watches the
  collectors — entirely through the database-level MON$RECORD_STATS
  counters, which count exactly the vio.cpp events the document
  describes:

      MON$RECORD_IMGC       — VIO_intermediate_gc collections (FB5+)
      MON$RECORD_PURGES     — purge():   chain trimmed, record lives on
      MON$RECORD_EXPUNGES   — expunge(): committed delete fully removed
      MON$BACKVERSION_READS — version-chain walks readers had to do

  Also prints the four header counters (OIT / OAT / OST / Next) around
  a rollback: isc_tpb_no_auto_undo goes straight into the TPB open
  array — a flag the node-firebird and rsfbclient twins could not set —
  so the rollback is recorded only in the TIP and the stump pins the
  OIT until sweep.  See ../../garbage-collection-and-sweep.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/gc_sweep && samples/fpc/bin/gc_sweep
}
program gc_sweep;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var W, P: IAttachment;
    Snap, T: ITransaction;
    i: integer;

{MON$ tables are a stable snapshot per transaction: use a fresh
 transaction for every peek so the counters are current.}
procedure ShowStats(const lbl: AnsiString);
var Tr: ITransaction;
    R: IResultSet;
begin
  Tr := W.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);
  R := W.OpenCursorAtStart(Tr,
    'select r.MON$RECORD_UPDATES upd, r.MON$RECORD_IMGC imgc, '
    + '       r.MON$RECORD_PURGES purges, r.MON$RECORD_EXPUNGES expunges, '
    + '       r.MON$BACKVERSION_READS backreads '
    + 'from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)');
  writeln(Format('%-34s upd=%-4s imgc=%-3s purges=%-3s expunges=%-3s backreads=%s',
    [lbl, R[0].AsString, R[1].AsString, R[2].AsString, R[3].AsString,
     R[4].AsString]));
  Tr.Commit;
end;

procedure ShowCounters(const lbl: AnsiString);
var Tr: ITransaction;
    R: IResultSet;
begin
  Tr := W.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);
  R := W.OpenCursorAtStart(Tr,
    'select MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$OLDEST_SNAPSHOT, '
    + '       MON$NEXT_TRANSACTION, MON$SWEEP_INTERVAL from MON$DATABASE');
  writeln(Format('%-34s OIT=%s OAT=%s OST=%s Next=%s (sweep interval %s)',
    [lbl, R[0].AsString, R[1].AsString, R[2].AsString, R[3].AsString,
     R[4].AsString]));
  Tr.Commit;
end;

function Val1(att: IAttachment; tr: ITransaction): AnsiString;
begin
  Result := att.OpenCursorAtStart(tr,
    'select val from gctest where id = 1')[0].AsString;
end;

begin
  W := AttachOrCreate(DbConn('gc_sweep'));
  P := FirebirdAPI.OpenDatabase(DbConn('gc_sweep'), DefaultDPB);

  try
    W.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
      'drop table gctest');
  except on EIBInterBaseError do ;
  end;
  W.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'create table gctest (id int primary key, val int)');
  W.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'insert into gctest values (1, 0)');

  { -- 1. Pin a snapshot: while this SNAPSHOT transaction lives, its
          tra_oldest_active holds the OST down and version 0 must survive. }
  Snap := P.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('pinned SNAPSHOT reads val = ', Val1(P, Snap));
  ShowStats('before updates:');

  { -- 2. Twelve committed updates -> twelve back versions... in theory. }
  for i := 1 to 12 do
    W.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
      'update gctest set val = ' + IntToStr(i) + ' where id = 1');
  ShowStats('after 12 updates (snapshot open):');
  writeln('pinned SNAPSHOT still reads val = ', Val1(P, Snap));

  { -- 3. Release the snapshot; a sequential scan now trips over the
          below-OST chain (cooperative GC) and/or notifies the GC thread. }
  Snap.Commit;
  T := W.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('snapshot released; new reader sees val = ', Val1(W, T));
  T.Commit;
  Sleep(1500);
  ShowStats('after release + scan + 1.5s:');

  { -- 4. A committed DELETE older than the OST is expunged, not purged. }
  W.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'delete from gctest where id = 1');
  T := W.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  W.OpenCursorAtStart(T, 'select count(*) from gctest');   { scan -> collect }
  T.Commit;
  Sleep(1500);
  ShowStats('after DELETE + scan + 1.5s:');

  { -- 5. A rolled-back transaction becomes an "interesting" stump: with
          no_auto_undo the rollback is recorded only in the TIP, so the
          OIT freezes there until a sweep rewrites its state. }
  ShowCounters('header counters before rollback:');
  T := W.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency,
    isc_tpb_no_auto_undo], taRollback);
  W.ExecuteSQL(T, 'insert into gctest values (2, 0)', []);
  T.Rollback;
  ShowCounters('after no_auto_undo rollback:');
  writeln('run ''gfix -sweep'' (or wait for OAT-OIT > interval) '
    + 'to move the OIT past the stump.');
end.
