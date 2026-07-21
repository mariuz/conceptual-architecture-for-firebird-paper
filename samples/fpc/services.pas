{
  services.pas — the Services API from Pascal: IServiceManager, an
  SRB-driven action, and the 1 KB ring-buffer polling loop.

  The fbintf twin of ../cpp/services.cpp — and the flagship of what this
  wrapper reaches: node-firebird and rsfbclient have NO Services API at
  all, while fbintf's IServiceManager wraps the real thing.  The sample
  attaches to the reserved service_mgr name (credentials in an SPB), asks
  the server for its version, then starts a VERBOSE backup of a scratch
  database.  The backup runs BURP_main — the real gbak — on a server
  thread; its output arrives through the 1 KB svc_stdout ring buffer,
  drained here with repeated IServiceManager.Query(isc_info_svc_line)
  calls.  Every path in the SRB is a SERVER path: the .fbk lands on the
  server's filesystem, owned by the server's user, and this client never
  touches either file.  See ../../services-api.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/services && samples/fpc/bin/services
}
program services;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;
    SPB: ISPB;
    Svc: IServiceManager;
    Req: ISRB;
    Results: IServiceQueryResults;
    BkPath, Line: AnsiString;
    lines, polls: integer;

begin
  BkPath := ChangeFileExt(DbPath('services'), '.fbk');   { server path, like the database }

  { 0. Make sure the scratch database exists (idempotent). }
  A := AttachOrCreate(DbConn('services'));
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);
  try
    A.ExecImmediate(Tr, 'create table t (id int, v varchar(20))');
  except on EIBInterBaseError do ;   { already there }
  end;
  Tr.Commit;
  A.Disconnect;

  { 1. Attach to the service manager: credentials in an SPB. }
  SPB := FirebirdAPI.AllocateSPB;
  SPB.Add(isc_spb_user_name).AsString := HandsOnUser;
  SPB.Add(isc_spb_password).AsString := HandsOnPassword;
  Svc := FirebirdAPI.GetServiceManager('localhost', TCP, SPB);

  { 2. Information request: no action, just server facts. }
  Req := Svc.AllocateSRB;
  Req.Add(isc_info_svc_server_version);
  Results := Svc.Query(Req);
  writeln('server version: ',
    Results.Find(isc_info_svc_server_version).AsString);

  { 3. Start action_backup — the services[] table dispatches this to
    BURP_main, i.e. gbak itself, on a server thread. }
  Req := Svc.AllocateSRB;
  Req.Add(isc_action_svc_backup);
  Req.Add(isc_spb_dbname).AsString := DbPath('services');   { server path! }
  Req.Add(isc_spb_bkp_file).AsString := BkPath;             { server path! }
  Req.Add(isc_spb_verbose);
  Svc.Start(Req);
  writeln('backup started (verbose) - draining the 1 KB ring buffer:');

  { 4. The polling loop: each Query() drains one isc_info_svc_line answer;
    the producer BLOCKS whenever the ring buffer is full, so a client
    that stops polling stalls the backup.  The empty line means the
    utility finished. }
  Req := Svc.AllocateSRB;
  Req.Add(isc_info_svc_line);
  lines := 0;
  polls := 0;
  repeat
    Results := Svc.Query(Req);
    inc(polls);
    Line := Results.Find(isc_info_svc_line).AsString;
    if Line <> '' then
    begin
      writeln('  ', Line);
      inc(lines);
    end;
  until Line = '';
  writeln('done: ', lines, ' gbak lines drained in ', polls, ' Query() polls');
  writeln('the file ', BkPath, ' now exists on the SERVER, owned by the server''s user');

  Svc.Detach;
  writeln;
  writeln('note: the Rust twin had to shell out to gbak here - rsfbclient has');
  writeln('no Services API.  node-firebird re-implements the service opcodes on');
  writeln('its own wire; fbintf drives libfbclient''s genuine service loop.');
  writeln('done.');
end.
