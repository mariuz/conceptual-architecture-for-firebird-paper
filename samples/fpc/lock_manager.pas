{
  lock_manager.pas — the lock-manager scenario in Pascal.

  The fbintf twin of ../cpp/lock_manager.cpp: drives the real lock
  manager and times all three wait outcomes.  Where the C++ twin issues
  "SET TRANSACTION ... RESERVING t1 FOR PROTECTED WRITE" as SQL, fbintf
  builds the same reservation directly in the TPB — the ITPB interface
  takes isc_tpb_lock_write with the table name plus isc_tpb_protected,
  and isc_tpb_lock_timeout with an integer argument — a genuine
  LCK_relation lock at LCK_EX, so the probes exercise
  enqueue/grant_or_que/wait_for_request, not the MVCC record-conflict
  path:

      NO WAIT        -> isc_lock_conflict, immediately   (lck_wait == 0)
      LOCK TIMEOUT 3 -> isc_lock_timeout, after ~3 s     (lck_wait < 0)
      WAIT           -> granted the moment the holder commits

  A final act builds a real deadlock through LCK_tra transaction locks
  and measures how long the periodic scanner (DeadlockTimeout = 10 s)
  takes to find it.  See ../../lock-manager.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/lock_manager && samples/fpc/bin/lock_manager
}
program lock_manager;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses {$IFDEF UNIX} cthreads, {$ENDIF} SysUtils, Classes, IB, FBHandsOn;

var A, B: IAttachment;
    Hold, TrA, TrB: ITransaction;
    AVictim, BVictim: boolean;
    T0: QWord;
    OutLock: TRTLCriticalSection;

{Threads and the main program share stdout: serialize and flush each line.}
procedure Say(const s: AnsiString);
begin
  EnterCriticalSection(OutLock);
  writeln(s);
  Flush(Output);
  LeaveCriticalSection(OutLock);
end;

function Secs(t0: QWord): double;
begin
  Result := (GetTickCount64 - t0) / 1000.0;
end;

function Flat(const s: AnsiString): AnsiString;
begin
  Result := StringReplace(Trim(s), #13, '', [rfReplaceAll]);
  Result := StringReplace(Result, #10, ' / ', [rfReplaceAll]);
  if Length(Result) > 110 then Result := Copy(Result, 1, 110) + '...';
end;

{The reservation TPB: <iso> [WAIT|NO WAIT|LOCK TIMEOUT n] RESERVING T1
 FOR PROTECTED WRITE, spelled in isc_tpb_* codes.}
function ReservingTPB(WaitMode: byte; LockTimeout: integer): ITPB;
begin
  Result := FirebirdAPI.AllocateTPB;
  Result.Add(isc_tpb_write);
  Result.Add(isc_tpb_concurrency);
  Result.Add(WaitMode);
  if LockTimeout > 0 then
    Result.Add(isc_tpb_lock_timeout).AsInteger := LockTimeout;
  Result.Add(isc_tpb_lock_write).AsString := 'T1';
  Result.Add(isc_tpb_protected);
end;

{Time one attempt to start a reserving transaction on B.}
procedure Probe(const lbl: AnsiString; WaitMode: byte; LockTimeout: integer);
var t0: QWord;
    t: ITransaction;
begin
  t0 := GetTickCount64;
  try
    t := B.StartTransaction(ReservingTPB(WaitMode, LockTimeout), taCommit);
    Say(Format('%-16s granted after %.3f s', [lbl, Secs(t0)]));
    t.Commit;
  except on E: EIBInterBaseError do
    Say(Format('%-16s failed after %.3f s: gds %d: %s',
      [lbl, Secs(t0), E.IBErrorCode, Flat(E.Message)]));
  end;
end;

type
  {Commits the holder's transaction 2 s from now, from another thread.}
  TReleaser = class(TThread)
  protected
    procedure Execute; override;
  end;

  {A's crossing update in the deadlock act.}
  TCrossA = class(TThread)
  protected
    procedure Execute; override;
  end;

procedure TReleaser.Execute;
begin
  Sleep(2000);
  Hold.Commit;
  Say('holder: committed (2 s later) -> lock released');
end;

procedure TCrossA.Execute;
begin
  try
    A.ExecuteSQL(TrA, 'update t1 set v = v + 1 where id = 2', []);
  except on E: EIBInterBaseError do
  begin                                 { this side was chosen as victim }
    Say(Format('deadlock: A failed after %.1f s: gds %d: %s',
      [Secs(T0), E.IBErrorCode, Flat(E.Message)]));
    AVictim := true;
    TrA.Rollback;                       { free B }
  end;
  end;
end;

var Releaser: TReleaser;
    CrossA: TCrossA;

begin
  A := AttachOrCreate(DbConn('lock_manager'));
  B := FirebirdAPI.OpenDatabase(DbConn('lock_manager'), DefaultDPB);

  { Scratch table (idempotent). }
  try
    A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
      isc_tpb_rec_version], 'drop table t1');
  except on EIBInterBaseError do ;
  end;
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'create table t1 (id int primary key, v int)');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'insert into t1 values (1, 0)');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'insert into t1 values (2, 0)');

  { A holds the LCK_relation lock at EX for the whole first act. }
  InitCriticalSection(OutLock);
  Hold := A.StartTransaction(ReservingTPB(isc_tpb_wait, 0), taCommit);
  Say('holder: t1 reserved FOR PROTECTED WRITE (LCK_relation at LCK_EX)');

  Probe('NO WAIT:', isc_tpb_nowait, 0);
  Probe('LOCK TIMEOUT 3:', isc_tpb_wait, 3);

  { WAIT parks in wait_for_request until the holder lets go: release the
    reservation from another thread after 2 s. }
  Releaser := TReleaser.Create(false);
  Probe('WAIT:', isc_tpb_wait, 0);
  Releaser.WaitFor;
  Releaser.Free;

  { Act two: a genuine wait-for cycle through LCK_tra locks.  Both sides
    block in WAIT mode; nobody looks for the cycle until the periodic
    scan fires - expect ~DeadlockTimeout seconds, not ~0. }
  Say('building deadlock: A updates row 1, B updates row 2, then cross...');
  TrA := A.StartTransaction([isc_tpb_write, isc_tpb_wait, isc_tpb_concurrency],
    taCommit);
  TrB := B.StartTransaction([isc_tpb_write, isc_tpb_wait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(TrA, 'update t1 set v = v + 1 where id = 1', []);
  B.ExecuteSQL(TrB, 'update t1 set v = v + 1 where id = 2', []);

  AVictim := false;
  BVictim := false;
  T0 := GetTickCount64;
  CrossA := TCrossA.Create(false);
  Sleep(300);
  try
    B.ExecuteSQL(TrB, 'update t1 set v = v + 1 where id = 1', []);
    Say(Format('deadlock: B''s update proceeded after %.1f s (A was the victim)',
      [Secs(T0)]));
  except on E: EIBInterBaseError do
  begin
    Say(Format('deadlock: B failed after %.1f s: gds %d: %s',
      [Secs(T0), E.IBErrorCode, Flat(E.Message)]));
    BVictim := true;
    TrB.Rollback;                       { free A }
  end;
  end;
  CrossA.WaitFor;
  CrossA.Free;
  if not AVictim then TrA.Rollback;
  if not BVictim then TrB.Rollback;
  DoneCriticalSection(OutLock);
  writeln('the wait is DeadlockTimeout (10 s default): the cycle sat undetected until the scan.');
  writeln('done.');
end.
