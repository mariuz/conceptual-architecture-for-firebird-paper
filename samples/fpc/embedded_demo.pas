{
  embedded_demo.pas — the full server engine, loaded into this process.

  The fbintf twin of ../cpp/embedded_demo.cpp.  Three demonstrations in
  one program:

    1. libfbclient is client AND engine — and fbintf makes the loading
       even more visible than the C++ twin: it dlopens libfbclient on
       first use of FirebirdAPI, so the process memory map shows THREE
       stages: nothing mapped, client mapped, then the first local-path
       attach makes the Y-valve pull in the Engine provider
       (plugins/libEngine14.so).
    2. Real work, no server: CREATE TABLE / INSERT / SELECT against a
       local .fdb owned by this process — DSQL, JRD, MVCC and careful
       write all on our own call stack (NETWORK_PROTOCOL is NULL).
       The embedded attach is just a plain filesystem path and no user
       name: authentication is the OS login, so the DPB carries only
       the connection charset.  The database lives in a user-writable
       directory (not the server's /tmp/fbhandson) because OUR process
       is the engine and needs to own the file.
    3. The continuum is measurable: attach/detach timed embedded vs
       remote — same fbintf API, same engine; the difference is the
       socket, the SRP handshake and a server round-trip per call.

  See ../../embedded-architecture-comparison.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/embedded_demo && samples/fpc/bin/embedded_demo
}
program embedded_demo;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

{Is a shared object whose name contains frag mapped into this process?}
function Mapped(const frag: AnsiString): boolean;
var f: TextFile;
    line: AnsiString;
begin
  Result := false;
  AssignFile(f, '/proc/self/maps');
  Reset(f);
  while not Eof(f) do
  begin
    ReadLn(f, line);
    if Pos(frag, line) > 0 then
    begin
      Result := true;
      break;
    end;
  end;
  CloseFile(f);
end;

function YN(b: boolean): AnsiString;
begin
  if b then Result := 'yes' else Result := 'no';
end;

procedure ShowMaps(const stage: AnsiString);
begin
  writeln(Format('%-20s libfbclient mapped=%-3s libEngine14 mapped=%s',
    [stage + ':', YN(Mapped('libfbclient')), YN(Mapped('libEngine14'))]));
end;

{Embedded DPB: no user name, no password — authentication is the OS
 login.  Only the connection charset.}
function EmbeddedDPB: IDPB;
begin
  Result := FirebirdAPI.AllocateDPB;
  Result.Add(isc_dpb_lc_ctype).setAsString('UTF8');
end;

var localDb, remoteDb: AnsiString;

function AttachMs(embedded: boolean): double;
var t0: TDateTime;
    att: IAttachment;
begin
  t0 := Now;
  if embedded then
    att := FirebirdAPI.OpenDatabase(localDb, EmbeddedDPB)
  else
    att := FirebirdAPI.OpenDatabase(remoteDb, DefaultDPB);
  att.Disconnect;
  Result := (Now - t0) * MSecsPerDay;
end;

const runs = 5;

var A: IAttachment;
    Tr: ITransaction;
    R: IResultSet;
    i: integer;
    emb, rem: double;

begin
  if ParamCount >= 1 then localDb := ParamStr(1)
  else localDb := GetEnvironmentVariable('HOME') + '/fbhandson/embedded_demo_fpc.fdb';
  if ParamCount >= 2 then remoteDb := ParamStr(2)
  else remoteDb := 'localhost:employee';
  ForceDirectories(ExtractFileDir(localDb));

  { -- 1. Watch the client, then the engine, arrive in our address space. }
  ShowMaps('before any API use');
  EmbeddedDPB;                       { first FirebirdAPI call dlopens the client }
  ShowMaps('after client load');
  A := FirebirdAPI.OpenDatabase(localDb, EmbeddedDPB, false);
  if A = nil then
    A := FirebirdAPI.CreateDatabase(localDb, EmbeddedDPB);
  ShowMaps('after local attach');
  writeln;

  { -- 2. Real work with no server anywhere. }
  try
    A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
      'drop table gadgets');
  except on EIBInterBaseError do ;
  end;
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'create table gadgets (id int primary key, name varchar(20))');
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(Tr, 'insert into gadgets values (1, ''sprocket'')', []);
  A.ExecuteSQL(Tr, 'insert into gadgets values (2, ''flange'')', []);
  A.ExecuteSQL(Tr, 'insert into gadgets values (3, ''grommet'')', []);
  Tr.CommitRetaining;

  R := A.OpenCursorAtStart(Tr,
    'select count(*), max(name), '
    + '       coalesce(rdb$get_context(''SYSTEM'', ''NETWORK_PROTOCOL''), '
    + '                ''<null: in-process>''), '
    + '       a.mon$server_pid '
    + 'from gadgets, mon$attachments a '
    + 'where a.mon$attachment_id = current_connection '
    + 'group by 3, 4');
  writeln('rows=', R[0].AsString, '  max(name)=', R[1].AsString,
    '  NETWORK_PROTOCOL=', R[2].AsString);
  writeln('engine pid=', R[3].AsString, ', my pid=', GetProcessID,
    ' - the ''server'' is this process');
  writeln;
  Tr.Commit;

  { -- 3. Attach cost: in-process call vs socket + SRP handshake.
          (A stays attached, like the C++ twin: the timed attaches join
          an already-open database rather than re-opening it each time.) }
  AttachMs(true);                    { warm-up (provider already loaded anyway) }
  AttachMs(false);                   { warm-up (socket/auth code paths) }
  emb := 0;
  rem := 0;
  for i := 1 to runs do
  begin
    emb := emb + AttachMs(true);
    rem := rem + AttachMs(false);
  end;
  writeln(Format('attach+detach avg over %d runs:', [runs]));
  writeln(Format('    embedded  %-42s %7.2f ms', [localDb, emb / runs]));
  writeln(Format('    remote    %-42s %7.2f ms', [remoteDb, rem / runs]));
  A.Disconnect;
  writeln('done.');
end.
