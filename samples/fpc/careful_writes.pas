{
  careful_writes.pas — kill a database engine mid-write, then just re-attach.

  The fbintf twin of ../cpp/careful_writes.cpp, companion to
  ../../careful-writes-and-crash-safety.md.  Uses the EMBEDDED engine
  (attach by plain local path, FIREBIRD=/opt/firebird) so that the child
  process this program spawns via TProcess IS the engine: SIGKILLing it
  while an uncommitted bulk insert is flushing dirty pages is a genuine
  engine crash, not just a dropped client connection.  The parent then
  re-attaches and verifies the careful-write guarantee: committed rows all
  present, uncommitted rows all gone, attach instantaneous — no log
  replay, because there is no log.

  Run against a scratch path only:
      make -C samples/fpc bin/careful_writes && samples/fpc/bin/careful_writes
}
program careful_writes;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, Process, BaseUnix, IB, FBHandsOn;

function setenv(name, value: PAnsiChar; overwrite: longint): longint;
  cdecl; external 'c';

const DEFAULT_DB = '/tmp/fbhandson/careful_writes_fpc.fdb';

function FileSizeOf(const path: AnsiString): int64;
var info: stat;
begin
  if FpStat(path, info) = 0 then
    Result := info.st_size
  else
    Result := -1;
end;

{Child mode: create the database, commit a marker, then bulk-insert half a
 million rows WITHOUT committing — and wait to be killed.}
procedure Writer(const dbFile: AnsiString);
var Att: IAttachment;
    Tr: ITransaction;
begin
  Att := AttachOrCreate(dbFile);           {plain path: the engine is US}

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  Att.ExecuteSQL(Tr, 'create table cw (id int, tag varchar(30))', []);
  Tr.Commit;

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  Att.ExecuteSQL(Tr, 'insert into cw values (1, ''committed-marker'')', []);
  Tr.Commit;
  writeln('[writer ', FpGetPid, '] marker row committed (forced writes on)');
  Flush(Output);

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_concurrency], taRollback);     {never completed: the crash victim}
  Att.ExecuteSQL(Tr,
    'execute block as declare i int = 0; begin' +
    '  while (i < 500000) do begin' +
    '    insert into cw values (:i + 1000, ''uncommitted''); i = i + 1;' +
    '  end ' +
    'end', []);
  writeln('[writer] bulk insert finished uncommitted; waiting for SIGKILL');
  Flush(Output);
  FpPause;
end;

var dbFile: AnsiString;
    Child: TProcess;
    base, sz: int64;
    i: integer;
    t0: QWord;
    committed, uncommitted: AnsiString;
    Att: IAttachment;
    Tr: ITransaction;

begin
  setenv('FIREBIRD', '/opt/firebird', 0);

  if (ParamCount >= 1) and (ParamStr(1) = '--writer') then
  begin
    Writer(ParamStr(2));
    exit;
  end;

  dbFile := DEFAULT_DB;
  if ParamCount >= 1 then dbFile := ParamStr(1);
  DeleteFile(dbFile);                      {fresh run}

  { 1. Spawn the writer: a separate process running the embedded engine. }
  Child := TProcess.Create(nil);
  Child.Executable := ParamStr(0);
  Child.Parameters.Add('--writer');
  Child.Parameters.Add(dbFile);
  Child.Execute;                           {stdout inherited, no pipes}

  { 2. Wait until the file is visibly growing — the engine is flushing
       freshly allocated data pages of the *uncommitted* transaction —
       then kill -9 the engine process mid-flight. }
  base := -1;
  for i := 1 to 600 do
  begin
    Sleep(50);
    sz := FileSizeOf(dbFile);
    if (base < 0) and (sz > 0) then
      base := sz;
    if (base > 0) and (sz > base + 2 * 1024 * 1024) then
    begin
      writeln('file grew ', base, ' -> ', sz, ' bytes; SIGKILL to engine pid ',
        Child.ProcessID);
      Flush(Output);
      break;
    end;
  end;
  FpKill(Child.ProcessID, SIGKILL);
  Child.WaitOnExit;
  Child.Free;

  { 3. Re-attach immediately.  No recovery step exists to run: the
       precedence graph never let an inconsistent state reach disk. }
  t0 := GetTickCount64;
  Att := FirebirdAPI.OpenDatabase(dbFile, DefaultDPB);
  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  committed := Att.OpenCursorAtStart(Tr,
    'select count(*) from cw where tag = ''committed-marker''')[0].AsString;
  uncommitted := Att.OpenCursorAtStart(Tr,
    'select count(*) from cw where tag = ''uncommitted''')[0].AsString;
  Tr.Commit;

  writeln('re-attach + both counts took ', GetTickCount64 - t0, ' ms');
  writeln('committed marker rows : ', committed, '   <- survived the crash');
  writeln('uncommitted rows      : ', uncommitted,
    '   <- rolled back by visibility, not replay');
end.
