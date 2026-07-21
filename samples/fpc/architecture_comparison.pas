{
  architecture_comparison.pas — one client library, two ways into the engine.

  The fbintf twin of ../cpp/architecture_comparison.cpp, companion to
  ../../architecture-comparison.md.  The comparison table's most unusual
  row is Firebird's: "client-server AND embedded, same engine".  One
  Pascal binary, linked only against libfbclient through fbintf, proves
  it: attach to localhost:employee and the Y-valve picks the Remote
  provider; attach to a plain local path and the Y-valve loads the Engine
  provider INTO THIS PROCESS - no server involved.  For each attachment
  the engine itself reports where it runs: ENGINE_VERSION,
  NETWORK_PROTOCOL, and MON$SERVER_PID versus our own pid.

  Build & run (see ../README.md):
      make -C samples/fpc bin/architecture_comparison
      samples/fpc/bin/architecture_comparison
}
program architecture_comparison;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, BaseUnix, IB, FBHandsOn;

{The embedded engine needs to find its root (plugins, security db).}
function setenv(name, value: PAnsiChar; overwrite: longint): longint;
  cdecl; external 'c';

procedure Inspect(const banner, conn: AnsiString; createIfAbsent: boolean);
var Att: IAttachment;
    Tr: ITransaction;
    R: IResultset;
    protocol, serverPid, samePid: AnsiString;
begin
  if createIfAbsent then
    Att := AttachOrCreate(conn)
  else
    Att := FirebirdAPI.OpenDatabase(conn, DefaultDPB);

  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  R := Att.OpenCursorAtStart(Tr,
    'select rdb$get_context(''SYSTEM'', ''ENGINE_VERSION''), ' +
    '       rdb$get_context(''SYSTEM'', ''NETWORK_PROTOCOL''), ' +
    '       a.mon$server_pid ' +
    'from mon$attachments a ' +
    'where a.mon$attachment_id = current_connection');

  if R[1].IsNull then protocol := '(null)' else protocol := R[1].AsString;
  serverPid := R[2].AsString;
  if serverPid = IntToStr(FpGetPid) then
    samePid := ' -- the engine runs IN this process'
  else
    samePid := '';

  writeln(banner);
  writeln('    connection string : ', conn);
  writeln('    ENGINE_VERSION    : ', R[0].AsString);
  writeln('    NETWORK_PROTOCOL  : ', protocol);
  writeln('    MON$SERVER_PID    : ', serverPid, '   (this process is pid ',
    FpGetPid, samePid, ')');
  Tr.Commit;
end;

var remoteDb, embeddedDb: AnsiString;

begin
  remoteDb := 'localhost:employee';
  if ParamCount >= 1 then remoteDb := ParamStr(1);
  embeddedDb := '/tmp/fbhandson/arch_embedded_fpc.fdb';
  if ParamCount >= 2 then embeddedDb := ParamStr(2);

  setenv('FIREBIRD', '/opt/firebird', 0);

  writeln('One libfbclient behind fbintf, two providers behind the Y-valve.');
  writeln;
  Inspect('[1] Remote provider (client-server):', remoteDb, false);
  writeln;
  Inspect('[2] Engine provider (embedded, no server):', embeddedDb, true);
  writeln;
  writeln('done.');
end.
