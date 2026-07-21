{
  trace.pas — a complete user trace session driven through fbintf's
  Services API.

  The fbintf twin of ../cpp/trace.cpp — and proof that fbintf, unlike
  the Rust twin (rsfbclient has no Services API at all), can walk the
  same path fbtracemgr takes:

    service A: isc_action_svc_trace_start with an inline configuration
               (isc_spb_trc_cfg); the service then streams the session's
               TraceLog back line by line (isc_info_svc_line);
    worker:    a TThread attaches to the traced database and runs one
               marker query — the observed side of the stream;
    service B: isc_action_svc_trace_stop ends the session, which ends
               A's stream.

  The trace configuration targets exactly one database, so the stream
  shows only the worker's doings: ATTACH, START_TRANSACTION, the
  statement with plan and per-table counters, COMMIT, DETACH.
  See ../../trace-and-audit.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/trace && samples/fpc/bin/trace
}
program trace;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses {$IFDEF UNIX} cthreads, {$ENDIF} SysUtils, Classes, IB, FBHandsOn;

var SessionID: integer;   { written by the stream reader, read by the worker }
    OutLock: TRTLCriticalSection;

{ Output is a threadvar in FPC, one buffer per thread: serialize whole
  lines AND flush before releasing the lock, or lines interleave. }
procedure SyncWrite(const s: AnsiString);
begin
  EnterCriticalSection(OutLock);
  writeln(s);
  Flush(Output);
  LeaveCriticalSection(OutLock);
end;

function TraceConfig: AnsiString;
begin
  Result :=
    'database = ' + DbPath('trace') + #10 +
    '{'#10 +
    '  enabled = true'#10 +
    '  log_connections = true'#10 +
    '  log_transactions = true'#10 +
    '  log_statement_finish = true'#10 +
    '  print_plan = true'#10 +
    '  print_perf = true'#10 +
    '  time_threshold = 0'#10 +
    '}'#10;
end;

function AttachSvc: IServiceManager;
var SPB: ISPB;
begin
  SPB := FirebirdAPI.AllocateSPB;
  SPB.Add(isc_spb_user_name).setAsString(HandsOnUser);
  SPB.Add(isc_spb_password).setAsString(HandsOnPassword);
  Result := FirebirdAPI.GetServiceManager('localhost', TCP, SPB);
end;

{ Fetch one service output line; false when the stream is finished. }
function NextLine(Svc: IServiceManager; var line: AnsiString): boolean;
var Req: ISRB;
    Results: IServiceQueryResults;
    Item: IServiceQueryResultItem;
begin
  Req := Svc.AllocateSRB;
  Req.Add(isc_info_svc_line);
  Results := Svc.Query(nil, Req);
  Item := Results.find(isc_info_svc_line);
  Result := (Item <> nil);
  if Result then
  begin
    line := Item.getAsString;
    Result := line <> '';
  end;
end;

type
  { The observed side, then the stop from a second service. }
  TWorker = class(TThread)
  protected
    procedure Execute; override;
  end;

procedure TWorker.Execute;
var Att: IAttachment;
    Tr: ITransaction;
    SvcB: IServiceManager;
    Req: ISRB;
    line: AnsiString;
begin
  Sleep(800);
  Att := FirebirdAPI.OpenDatabase(DbConn('trace'), DefaultDPB);
  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  SyncWrite('[worker] marker query says: ' + Trim(Att.OpenCursorAtStart(Tr,
    'SELECT COUNT(*) FROM RDB$RELATIONS /* traced! */')[0].AsString));
  Tr.Commit;
  Att := nil;                             { DETACH, while the session runs }

  Sleep(1200);
  while SessionID = 0 do                  { wait for the id from the stream }
    Sleep(50);
  SvcB := AttachSvc;
  Req := SvcB.AllocateSRB;
  Req.Add(isc_action_svc_trace_stop);
  Req.Add(isc_spb_trc_id).SetAsInteger(SessionID);
  SvcB.Start(Req);
  while NextLine(SvcB, line) do
    SyncWrite('[stop ] ' + TrimRight(line));
  SvcB.Detach;
end;

var Att: IAttachment;
    SvcA: IServiceManager;
    Req: ISRB;
    Worker: TWorker;
    line: AnsiString;
    p: integer;

begin
  SessionID := 0;
  InitCriticalSection(OutLock);

  { The database to be observed must exist before the session starts. }
  Att := AttachOrCreate(DbConn('trace'));
  Att := nil;

  { -- service A: start the trace session, config passed inline ------- }
  SvcA := AttachSvc;
  Req := SvcA.AllocateSRB;
  Req.Add(isc_action_svc_trace_start);
  Req.Add(isc_spb_trc_name).SetAsString('hands-on');
  Req.Add(isc_spb_trc_cfg).SetAsString(TraceConfig);
  SvcA.Start(Req);

  Worker := TWorker.Create(false);

  { -- service A's stream: the trace output itself -------------------- }
  while NextLine(SvcA, line) do
  begin
    SyncWrite('[trace] ' + TrimRight(line));
    if (SessionID = 0) and (Pos('Trace session ID ', line) = 1) then
      if TryStrToInt(Trim(Copy(line, Length('Trace session ID ') + 1,
          Pos(' started', line) - Length('Trace session ID ') - 1)), p) then
        SessionID := p;
  end;

  Worker.WaitFor;
  Worker.Free;
  SvcA.Detach;
  DoneCriticalSection(OutLock);
  writeln('done.');
end.
