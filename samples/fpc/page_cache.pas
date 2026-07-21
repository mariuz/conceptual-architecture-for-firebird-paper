{
  page_cache.pas — one shared cache vs two private caches.

  The fbintf twin of ../cpp/page_cache.cpp: the same hot-page ping-pong
  run against both cache topologies, refereed by per-attachment
  MON$IO_STATS:

    phase 1 — two client processes -> ONE SuperServer shared cache
              (coherency by shared memory; almost no physical I/O)
    phase 2 — two EMBEDDED engine processes with PRIVATE caches over one
              file (ServerMode=SuperClassic sandbox; coherency by LCK_bdb
              page locks + blocking ASTs — data travels through the disk,
              so the same workload turns into hundreds of reads AND writes)

  The parent only spawns children (TProcess re-invoking this binary with a
  role argument — the Pascal spelling of the C++ twin's fork/exec); every
  attachment lives in a child process, and each embedded child IS a full
  engine because its connection string has no host part.  Rows 1 and 2
  share a data page, so the two writers fight over one page without ever
  touching one row.  See ../../page-cache-coherency.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/page_cache && samples/fpc/bin/page_cache
}
program page_cache;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, Classes, Process, BaseUnix, IB, FBHandsOn;

const
  SRV_DB = 'localhost:/tmp/fbhandson/page_cache_srv_fpc.fdb';
  EMB_DB = '/tmp/fbhandson/page_cache_emb_fpc.fdb';
  SANDBOX = '/tmp/fbhandson/fbemb_fpc';
  ROUNDS = 300;

var UseSandboxEnv: boolean = false;   { phase 2: children get FIREBIRD=SANDBOX }

procedure InitDb(const conn: AnsiString);      { --init: fresh table, two rows }
var A: IAttachment;
    T: ITransaction;
begin
  A := AttachOrCreate(conn);
  T := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  try A.ExecuteSQL(T, 'drop table t', []);
  except on EIBInterBaseError do ; end;
  T.Commit;
  T := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(T, 'create table t (id int primary key, v int)', []);
  T.Commit;
  T := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(T, 'insert into t values (1, 0)', []);
  A.ExecuteSQL(T, 'insert into t values (2, 0)', []);
  T.Commit;
end;

procedure Worker(const conn, rowId: AnsiString);              { --worker }
var A: IAttachment;
    T: ITransaction;
    R: IResultset;
    i: integer;
begin
  A := FirebirdAPI.OpenDatabase(conn, DefaultDPB);
  for i := 1 to ROUNDS do
  begin
    T := A.StartTransaction([isc_tpb_write, isc_tpb_wait, isc_tpb_concurrency],
      taCommit);
    A.ExecuteSQL(T, 'update t set v = v + 1 where id = ' + rowId, []);
    T.Commit;
  end;
  T := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  R := A.OpenCursorAtStart(T,
    'select MON$PAGE_FETCHES, MON$PAGE_READS, MON$PAGE_WRITES ' +
    'from MON$IO_STATS join MON$ATTACHMENTS using (MON$STAT_ID) ' +
    'where MON$ATTACHMENT_ID = CURRENT_CONNECTION');
  writeln(Format('  worker pid %-6d row %s: %d commits | page fetches=%-6s ' +
    'reads=%-4s writes=%s', [FpGetPid, rowId, ROUNDS,
    R[0].AsString, R[1].AsString, R[2].AsString]));
  T.Commit;
end;

procedure Check(const conn: AnsiString);       { --check: any lost updates? }
var A: IAttachment;
    T: ITransaction;
    R: IResultset;
begin
  A := FirebirdAPI.OpenDatabase(conn, DefaultDPB);
  T := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  R := A.OpenCursorAtStart(T, 'select id, v from t order by id');
  while not R.IsEof do
  begin
    writeln(Format('  final: id=%s v=%s (expected %d)',
      [R[0].AsString, R[1].AsString, ROUNDS]));
    R.FetchNext;
  end;
  T.Commit;
end;

function Spawn(const args: array of AnsiString): TProcess;
var i: integer;
begin
  Result := TProcess.Create(nil);
  Result.Executable := ParamStr(0);
  for i := 0 to High(args) do
    Result.Parameters.Add(args[i]);
  if UseSandboxEnv then
  begin  { children inherit our environment plus the sandbox FIREBIRD root }
    for i := 1 to GetEnvironmentVariableCount do
      Result.Environment.Add(GetEnvironmentString(i));
    Result.Environment.Add('FIREBIRD=' + SANDBOX);
  end;
  Result.Execute;
end;

procedure Run1(const args: array of AnsiString);   { spawn and await one }
var P: TProcess;
begin
  Flush(Output);
  P := Spawn(args);
  P.WaitOnExit;
  P.Free;
end;

procedure RunWorkers(const conn: AnsiString);      { two writers in parallel }
var P1, P2: TProcess;
begin
  Flush(Output);
  P1 := Spawn(['--worker', conn, '1']);
  P2 := Spawn(['--worker', conn, '2']);
  P1.WaitOnExit;
  P2.WaitOnExit;
  P1.Free;
  P2.Free;
end;

{ Build the embedded sandbox: a FIREBIRD root whose firebird.conf says
  SuperClassic, so each process locks the file SHARED and runs its own
  page cache (see 'Three layers of arbitration'). }
procedure MakeSandbox;
const links: array[0..4] of AnsiString = ('plugins', 'intl', 'tzdata',
    'firebird.msg', 'security6.fdb');
var i: integer;
    conf: TextFile;
begin
  FpMkdir(SANDBOX, &777);
  for i := 0 to High(links) do
    FpSymlink(PAnsiChar('/opt/firebird/' + links[i]),
      PAnsiChar(SANDBOX + '/' + links[i]));
  AssignFile(conf, SANDBOX + '/firebird.conf');
  Rewrite(conf);
  writeln(conf, 'ServerMode = SuperClassic');
  CloseFile(conf);
end;

begin
  if (ParamCount >= 2) and (ParamStr(1) = '--init') then
    begin InitDb(ParamStr(2)); exit; end;
  if (ParamCount >= 3) and (ParamStr(1) = '--worker') then
    begin Worker(ParamStr(2), ParamStr(3)); exit; end;
  if (ParamCount >= 2) and (ParamStr(1) = '--check') then
    begin Check(ParamStr(2)); exit; end;

  writeln('phase 1: two client processes, ONE SuperServer shared cache');
  Run1(['--init', SRV_DB]);
  RunWorkers(SRV_DB);
  Run1(['--check', SRV_DB]);

  MakeSandbox;
  DeleteFile(EMB_DB);

  writeln('phase 2: two EMBEDDED engine processes, PRIVATE page caches');
  UseSandboxEnv := true;
  Run1(['--init', EMB_DB]);
  RunWorkers(EMB_DB);
  Run1(['--check', EMB_DB]);
  writeln('same workload - the private caches paid for coherency in disk I/O.');
end.
