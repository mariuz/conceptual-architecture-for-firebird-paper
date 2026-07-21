{
  threading.pas — SuperServer's thread-per-attachment topology, via fbintf.

  The fbintf twin of ../cpp/threading.cpp: watch the engine's thread
  census from the outside.  MON$SERVER_PID names the engine process;
  /proc/<pid>/task counts its threads before, during and after twelve
  concurrent attachments opened from twelve TThreads of THIS process —
  one fbintf IAttachment per thread, the supported multi-threading
  pattern.  MON$ATTACHMENTS shows the engine's own background workers
  (Cache Writer, Garbage Collector) as system attachments, and every
  attachment reports the same MON$SERVER_PID: one process, many threads
  — ServerMode = Super in action.
  See ../../threading-and-synchronization.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/threading && samples/fpc/bin/threading
}
program threading;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses {$IFDEF UNIX} cthreads, {$ENDIF} SysUtils, Classes, IB, FBHandsOn;

const DDL_TPB: array[0..3] of byte = (isc_tpb_write, isc_tpb_nowait,
        isc_tpb_read_committed, isc_tpb_rec_version);

type
  { One attachment per thread: fbintf interfaces must not be shared
    across threads without care, so each worker opens its own. }
  TWorker = class(TThread)
  protected
    procedure Execute; override;
  end;

procedure TWorker.Execute;
var W: IAttachment;
    Tr: ITransaction;
begin
  W := FirebirdAPI.OpenDatabase(DbConn('threading'), DefaultDPB);
  Tr := W.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  W.OpenCursorAtStart(Tr, 'select count(*) from t');
  Tr.Commit;
  Sleep(2000);        { hold the attachment open: one server thread each }
end;

function CountThreads(const pid: AnsiString): integer;
var sr: TSearchRec;
begin
  Result := 0;
  if FindFirst('/proc/' + pid + '/task/*', faAnyFile, sr) = 0 then
  begin
    repeat
      if sr.Name[1] <> '.' then
        Inc(Result);
    until FindNext(sr) <> 0;
    FindClose(sr);
  end;
end;

function Pad(const s: AnsiString; w: integer): AnsiString;
begin
  Result := s;
  while Length(Result) < w do
    Result := Result + ' ';
end;

var Att: IAttachment;
    Tr: ITransaction;
    Cur: IResultSet;
    pid, users, pids: AnsiString;
    workers: array[0..11] of TWorker;
    i: integer;

begin
  Att := AttachOrCreate(DbConn('threading'));

  Att.ExecImmediate(DDL_TPB, 'recreate table t (id int primary key, v int)');
  Att.ExecImmediate(DDL_TPB, 'update or insert into t values (1, 0) matching (id)');

  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  pid := Trim(Att.OpenCursorAtStart(Tr,
    'select MON$SERVER_PID from MON$ATTACHMENTS ' +
    'where MON$ATTACHMENT_ID = current_connection')[0].AsString);
  Tr.Commit;
  writeln('engine process: pid ', pid, ', ', CountThreads(pid),
    ' threads (1 attachment open)');

  { Twelve attachments from twelve threads of THIS process; each holds
    its attachment open for 2 s.  Server side: one thread each (drawn
    from the pool when idle threads exist, created otherwise). }
  for i := 0 to High(workers) do
    workers[i] := TWorker.Create(false);

  Sleep(1000);
  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  users := Trim(Att.OpenCursorAtStart(Tr,
    'select count(*) from MON$ATTACHMENTS where MON$SYSTEM_FLAG = 0')[0].AsString);
  pids := Trim(Att.OpenCursorAtStart(Tr,
    'select count(distinct MON$SERVER_PID) from MON$ATTACHMENTS')[0].AsString);
  Tr.Commit;
  writeln('with 12 extra attachments: ', CountThreads(pid), ' threads | ',
    users, ' user attachments, ', pids, ' distinct server pid');

  for i := 0 to High(workers) do
  begin
    workers[i].WaitFor;
    workers[i].Free;
  end;
  Sleep(1000);
  writeln('after they detach:        ', CountThreads(pid),
    ' threads (pooled, not destroyed)');

  { The engine's own workers hold real attachments, visible from SQL. }
  writeln;
  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  Cur := Att.OpenCursor(Tr,
    'select MON$ATTACHMENT_ID, MON$SYSTEM_FLAG, trim(MON$USER) as MON$USER, ' +
    '       coalesce(MON$REMOTE_PROCESS, ''<internal>'') as MON$REMOTE_PROCESS ' +
    'from MON$ATTACHMENTS order by MON$ATTACHMENT_ID');
  writeln(Pad('ID', 5), Pad('SYS', 4), Pad('USER', 22), 'REMOTE_PROCESS');
  writeln(Pad('--', 5), Pad('---', 4), Pad('----', 22), '--------------');
  while Cur.FetchNext do
    writeln(Pad(Cur[0].AsString, 5), Pad(Cur[1].AsString, 4),
      Pad(Trim(Cur[2].AsString), 22), Cur[3].AsString);
  Cur.Close;
  Tr.Commit;

  writeln;
  writeln('done.');
end.
