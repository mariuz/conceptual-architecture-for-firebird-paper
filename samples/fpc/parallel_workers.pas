{
  parallel_workers.pas — watching worker attachments appear (and be refused).

  The fbintf twin of ../cpp/parallel_workers.cpp, same two phases:

  [A] Against the live server: ask for 4 workers via isc_dpb_parallel_workers.
      fbintf has no name for that DPB tag (its constants stop at Firebird 4),
      but IDPB.Add takes a raw byte, so the FB5 tag passes straight through —
      the same escape hatch the fb-cpp twin used.  With the stock
      MaxParallelWorkers = 1 the engine clamps the request and attaches with
      a WARNING.  One fbintf gap: its status handling raises exceptions on
      errors but discards warnings, so the isc_bad_par_workers warning the
      OO-API twin fishes out of the status vector is invisible here — the
      clamp is read back from MON$ATTACHMENTS.MON$PARALLEL_WORKERS instead.

  [B] Against an embedded engine whose private FIREBIRD root sets
      ParallelWorkers = 4 / MaxParallelWorkers = 8: build a wide 200k-row
      table, CREATE INDEX on it, and poll MON$ATTACHMENTS from a second
      attachment while the build runs.  The workers appear as ordinary
      attachments — MON$USER = '<Worker>', MON$SYSTEM_FLAG = 1 — and stay
      pooled (idle timeout 60 s) after the build: parallelism built out of
      attachments, exactly as ../../parallel-workers.md argues.

  Build & run (see ../README.md):
      make -C samples/fpc bin/parallel_workers && samples/fpc/bin/parallel_workers
      # ~30 s: builds a 40 MB scratch table
}
program parallel_workers;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses cthreads, SysUtils, Classes, BaseUnix, IB, FBHandsOn;

const
  ROOT = '/tmp/fbhandson/fbroot-parallel-fpc';
  REMOTE_DB = 'localhost:/tmp/fbhandson/parallel_fpc.fdb';
  EMBEDDED_DB = '/tmp/fbhandson/par_embedded_fpc.fdb';
  { Firebird 5 DPB tag, newer than fbintf's constant tables (which end at
    isc_dpb_decfloat_traps = 87) — IDPB.Add passes the raw byte through. }
  isc_dpb_parallel_workers = 100;

{ setenv(3): the FIREBIRD variable must reach the engine library that
  fbclient loads in-process for phase B.  The FPC RTL has no portable
  setter, so take it from libc. }
function setenv(name, value: PAnsiChar; overwrite: longint): longint;
  cdecl; external 'c';

type
  TPoller = class(TThread)
  public
    MaxSeen: integer;
    Roster: AnsiString;
    procedure Execute; override;
  end;

procedure TPoller.Execute;
var Mon: IAttachment;
    T: ITransaction;
    R: IResultset;
    n: integer;
begin
  try
    Mon := FirebirdAPI.OpenDatabase(EMBEDDED_DB, DefaultDPB);
    while not Terminated do
    begin
      T := Mon.StartTransaction([isc_tpb_read, isc_tpb_nowait,
        isc_tpb_concurrency], taCommit);      { fresh MON$ snapshot }
      n := Mon.OpenCursorAtStart(T,
        'select count(*) from mon$attachments ' +
        'where mon$user = ''<Worker>''')[0].AsInteger;
      if n > MaxSeen then
      begin
        MaxSeen := n;
        Roster := '';
        R := Mon.OpenCursorAtStart(T,
          'select trim(mon$user), mon$system_flag ' +
          'from mon$attachments order by mon$attachment_id');
        while not R.IsEof do
        begin
          Roster := Roster + '        ' + R[0].AsString +
            '  (system_flag ' + R[1].AsString + ')' + #10;
          R.FetchNext;
        end;
      end;
      T.Commit;
      Sleep(20);
    end;
  except
    { attachment races at shutdown are harmless here }
  end;
end;

{ A private $FIREBIRD root: symlinks into the stock install, own firebird.conf. }
procedure MakeRoot;
const links: array[0..5] of AnsiString = ('plugins', 'intl', 'firebird.msg',
    'tzdata', 'plugins.conf', 'databases.conf');
var i: integer;
    conf: TextFile;
begin
  FpMkdir(ROOT, &775);
  for i := 0 to High(links) do
    FpSymlink(PAnsiChar('/opt/firebird/' + links[i]),
      PAnsiChar(ROOT + '/' + links[i]));
  AssignFile(conf, ROOT + '/firebird.conf');
  Rewrite(conf);
  writeln(conf, 'ServerMode = Super');
  writeln(conf, 'ParallelWorkers = 4');
  writeln(conf, 'MaxParallelWorkers = 8');
  CloseFile(conf);
end;

function Knobs(att: IAttachment; tr: ITransaction): AnsiString;
begin
  Result := 'ParallelWorkers = ' + att.OpenCursorAtStart(tr,
      'select rdb$config_value from rdb$config ' +
      'where rdb$config_name = ''ParallelWorkers''')[0].AsString +
    ', MaxParallelWorkers = ' + att.OpenCursorAtStart(tr,
      'select rdb$config_value from rdb$config ' +
      'where rdb$config_name = ''MaxParallelWorkers''')[0].AsString;
end;

var A: IAttachment;
    Tr: ITransaction;
    DPB: IDPB;
    Poller: TPoller;
    t0: QWord;

begin
  setenv('FIREBIRD', ROOT, 1);  { read by the embedded engine of phase B }
  MakeRoot;

  { --- [A] the server refuses politely --------------------------------- }
  A := AttachOrCreate(REMOTE_DB);  { ensure it exists }
  A := nil;

  DPB := DefaultDPB;
  DPB.Add(isc_dpb_parallel_workers).SetAsInteger(4);
  A := FirebirdAPI.OpenDatabase(REMOTE_DB, DPB);

  writeln('[A] server attach, isc_dpb_parallel_workers = 4');
  writeln('    (fbintf discards status-vector warnings, so the');
  writeln('    isc_bad_par_workers warning is invisible - asking MON$ instead)');
  Tr := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('    granted: MON$PARALLEL_WORKERS = ', A.OpenCursorAtStart(Tr,
    'select mon$parallel_workers from mon$attachments ' +
    'where mon$attachment_id = current_connection')[0].AsString);
  writeln('    server config: ', Knobs(A, Tr),
    ' -> request clamped, 0 extra workers');
  writeln;
  Tr.Commit;
  A := nil;

  { --- [B] embedded engine with its own firebird.conf ------------------ }
  A := AttachOrCreate(EMBEDDED_DB);
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('[B] embedded attach, FIREBIRD=', ROOT);
  writeln('    engine config: ', Knobs(A, Tr));

  try A.ExecuteSQL(Tr, 'drop table parade', []);
  except on EIBInterBaseError do ; end;
  A.ExecuteSQL(Tr, 'create table parade (id int, val varchar(200))', []);
  Tr.CommitRetaining;
  { The filler must be incompressible: records are RLE-compressed on
    page, and IndexCreateTask::getMaxWorkers() goes parallel only if
    the relation spans more than one pointer page. }
  A.ExecuteSQL(Tr,
    'execute block as declare n int = 0; begin ' +
    '  while (n < 200000) do begin ' +
    '    insert into parade values (:n, ' +
    '      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || ' +
    '      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || ' +
    '      uuid_to_char(gen_uuid())); ' +
    '    n = n + 1; ' +
    '  end end', []);
  Tr.CommitRetaining;
  writeln('    parade table: 200000 rows of 180 incompressible bytes, ',
    A.OpenCursorAtStart(Tr,
      'select count(*) from rdb$pages p join rdb$relations r ' +
      '  on p.rdb$relation_id = r.rdb$relation_id ' +
      'where r.rdb$relation_name = ''PARADE'' and p.rdb$page_type = 4')
      [0].AsString, ' pointer pages');

  Poller := TPoller.Create(false);
  t0 := GetTickCount64;
  A.ExecuteSQL(Tr, 'create index ix_parade on parade (val)', []);
  Tr.CommitRetaining;
  Poller.Terminate;
  Poller.WaitFor;

  writeln('    create index: ', GetTickCount64 - t0,
    ' ms; max ''<Worker>'' attachments seen: ', Poller.MaxSeen);
  writeln('    MON$ATTACHMENTS at the widest moment:');
  write(Poller.Roster);
  writeln('    after build: workers stay pooled (idle timeout 60 s): ',
    A.OpenCursorAtStart(Tr,
      'select count(*) from mon$attachments ' +
      'where mon$user = ''<Worker>''')[0].AsString);
  Tr.Commit;
  Poller.Free;
  writeln('done.');
end.
