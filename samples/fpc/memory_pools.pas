{
  memory_pools.pas — the memory-management scenario in Pascal.

  The fbintf twin of ../cpp/memory_pools.cpp: makes the pool hierarchy
  visible from SQL via MON$MEMORY_USAGE —

    - the per-level summary (stat groups) with the parent-redirection
      signature: child pools showing real MON$MEMORY_USED and *zero*
      MON$MEMORY_ALLOCATED — every block borrowed from the database pool;
    - this connection's own database -> attachment -> transaction chain;
    - a transaction pool growing live while its transaction accumulates
      an undo log (an uncommitted 3000-row UPDATE), observed from a
      second attachment because MON$ snapshots are per-transaction.

  See ../../memory-management.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/memory_pools && samples/fpc/bin/memory_pools
}
program memory_pools;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var Worker, Monitor: IAttachment;
    Tr: ITransaction;
    AttId, TraId: AnsiString;

{Per-level summary in a fresh monitor transaction (fresh MON$ snapshot).}
procedure LevelSummary;
var t: ITransaction;
    rs: IResultSet;
begin
  t := Monitor.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  rs := Monitor.OpenCursor(t,
    'select MON$STAT_GROUP, count(*), sum(MON$MEMORY_USED), '
    + '     sum(MON$MEMORY_ALLOCATED), '
    + '     count(nullif(MON$MEMORY_ALLOCATED, 0)) '
    + 'from MON$MEMORY_USAGE group by 1 order by 1');
  writeln('stat_group (0=db 1=att 2=tra 3=stmt 5=cmp)  pools  used  allocated  with_own_extents');
  while rs.FetchNext do
    writeln('  ', rs[0].AsString, '  ', rs[1].AsString:5, '  ', rs[2].AsString:9,
      '  ', rs[3].AsString:9, '  ', rs[4].AsString:3);
  rs.Close;
  t.Commit;
end;

{One row of the worker's own pool chain, freshly snapshotted by the monitor.}
procedure PoolRow(const lbl, join: AnsiString);
var t: ITransaction;
    rs: IResultSet;
begin
  t := Monitor.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  rs := Monitor.OpenCursor(t,
    'select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE ' + join);
  if rs.FetchNext then
    writeln(Format('  %-24s used=%-10s allocated=%s',
      [lbl, rs[0].AsString, rs[1].AsString]));
  rs.Close;
  t.Commit;
end;

begin
  Worker := AttachOrCreate(DbConn('memory_pools'));
  Monitor := FirebirdAPI.OpenDatabase(DbConn('memory_pools'), DefaultDPB);

  Worker.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'recreate table t (id int, pad varchar(200))');
  Worker.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version],
    'execute block as declare i int = 0; begin'
    + '  while (i < 3000) do begin'
    + '    insert into t values (:i, rpad(''x'', 200, ''x'')); i = i + 1;'
    + '  end '
    + 'end');

  writeln('-- per-level summary (note used > 0 with allocated = 0: parent redirection)');
  LevelSummary;

  { The worker's own chain: database -> attachment -> transaction. }
  Tr := Worker.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  AttId := Worker.OpenCursorAtStart(Tr,
    'select current_connection from rdb$database')[0].AsString;
  TraId := Worker.OpenCursorAtStart(Tr,
    'select current_transaction from rdb$database')[0].AsString;

  writeln;
  writeln('-- worker''s pool chain (before the update)');
  PoolRow('database pool:', 'join MON$DATABASE using (MON$STAT_ID)');
  PoolRow('worker attachment pool:',
    'join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ' + AttId);
  PoolRow('worker transaction pool:',
    'join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = ' + TraId);

  { Grow the transaction pool: an uncommitted UPDATE of 3000 rows must keep
    every old version in this transaction's undo log, and the undo log
    lives in the transaction's pool. }
  Worker.ExecuteSQL(Tr, 'update t set pad = rpad(''y'', 200, ''y'')', []);

  writeln;
  writeln('-- after an uncommitted 3000-row UPDATE in that transaction');
  PoolRow('worker attachment pool:',
    'join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ' + AttId);
  PoolRow('worker transaction pool:',
    'join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = ' + TraId);

  Tr.Rollback;    { bulk-free: the whole pool goes at once }
  writeln;
  writeln('-- after rollback (transaction pool destroyed with its undo log)');
  PoolRow('worker attachment pool:',
    'join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ' + AttId);

  writeln;
  writeln('done.');
end.
