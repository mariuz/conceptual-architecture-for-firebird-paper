{
  pooling.pas — the external-connections (EDS) pool, watched live.

  The fbintf twin of ../cpp/pooling.cpp: the server acts as a CLIENT of
  another data source via EXECUTE STATEMENT ... ON EXTERNAL, and the
  engine pools those outbound connections.  The pool is tuned at runtime
  (ALTER EXTERNAL CONNECTIONS POOL), three external calls share ONE
  outbound connection, and the pool's four SYSTEM context variables are
  read back at each stage:

      before:            idle 0, active 0
      inside the block:  idle 0, active 1   <- three calls, ONE connection
      after commit:      idle 1, active 0   <- reset, back on the idle list
      after CLEAR ALL:   idle 0             <- evicted

  Everything is plain SQL through IAttachment.ExecuteSQL/OpenCursorAtStart;
  CommitRetaining keeps the transaction context (and with it the ACTIVE
  pooled connection) alive, exactly as the C++ twin demonstrates.
  See ../../connection-pooling.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/pooling && samples/fpc/bin/pooling
}
program pooling;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;
    R: IResultset;
    database, external_: AnsiString;
    block: AnsiString;

procedure PoolState(const moment: AnsiString);
var S: IResultset;
begin
  S := A.OpenCursorAtStart(Tr,
    'select rdb$get_context(''SYSTEM'', ''EXT_CONN_POOL_SIZE''),' +
    '       rdb$get_context(''SYSTEM'', ''EXT_CONN_POOL_LIFETIME''),' +
    '       rdb$get_context(''SYSTEM'', ''EXT_CONN_POOL_IDLE_COUNT''),' +
    '       rdb$get_context(''SYSTEM'', ''EXT_CONN_POOL_ACTIVE_COUNT'')' +
    ' from rdb$database');
  writeln(Format('%-18s size=%s lifetime=%ss idle=%s active=%s',
    [moment, S[0].AsString, S[1].AsString, S[2].AsString, S[3].AsString]));
end;

begin
  if ParamCount >= 1 then database := ParamStr(1)
  else database := 'localhost:employee';
  if ParamCount >= 2 then external_ := ParamStr(2)
  else external_ := 'localhost:employee';   { the DSN the SERVER connects to }

  A := FirebirdAPI.OpenDatabase(database, DefaultDPB);

  { 1. Tune the pool at runtime (per server process, not persistent). }
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(Tr, 'alter external connections pool set size 5', []);
  A.ExecuteSQL(Tr, 'alter external connections pool set lifetime 30 second', []);
  Tr.CommitRetaining;

  PoolState('before:');

  { 2. Three EXECUTE STATEMENT ON EXTERNAL calls to the SAME
       (connection string, user, password, role) — the pool's key. }
  block :=
    'execute block returns (idle varchar(10), active varchar(10)) as'#10 +
    '  declare i int = 0;'#10 +
    '  declare v int;'#10 +
    'begin'#10 +
    '  while (i < 3) do'#10 +
    '  begin'#10 +
    '    execute statement ''select 1 from rdb$database'''#10 +
    '      on external ''' + external_ + ''''#10 +
    '      as user ''' + HandsOnUser + ''' password ''' + HandsOnPassword + ''''#10 +
    '      into :v;'#10 +
    '    i = i + 1;'#10 +
    '  end'#10 +
    '  idle   = rdb$get_context(''SYSTEM'', ''EXT_CONN_POOL_IDLE_COUNT'');'#10 +
    '  active = rdb$get_context(''SYSTEM'', ''EXT_CONN_POOL_ACTIVE_COUNT'');'#10 +
    '  suspend;'#10 +
    'end';
  R := A.OpenCursorAtStart(Tr, block);
  writeln(Format('%-18s idle=%s active=%s   (3 calls, 1 outbound connection)',
    ['inside the block:', R[0].AsString, R[1].AsString]));

  { 3. Full commit: only now is the external connection truly unused —
       it is reset with ALTER SESSION RESET and parked on the idle
       list.  (COMMIT RETAINING keeps the transaction context alive,
       and with it the pooled connection stays ACTIVE — try it.) }
  Tr.Commit;
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  PoolState('after commit:');

  { 4. Evict every idle connection now. }
  A.ExecuteSQL(Tr, 'alter external connections pool clear all', []);
  PoolState('after CLEAR ALL:');

  Tr.Commit;
  writeln('done.');
end.
