{
  sorting.pas — the TempCacheLimit threshold made visible, via fbintf.

  The fbintf twin of ../cpp/sorting.cpp.  Fills a table with 200,000
  rows (~82 MB of sort data with a 400-byte key), then runs two ORDER BY
  queries:

    big sort   200,000 rows  ->  ~82 MB  >  TempCacheLimit (64 MB)  -> spills
    small sort  20,000 rows  ->   ~8 MB  <  TempCacheLimit          -> stays in RAM

  While each query runs, a watcher TThread (with its own IAttachment —
  MON$ polling needs a second connection) samples two things:
    -  the server's /proc/<pid>/fd table (via sudo) for fb_sort_*
       scratch files — unlinked on creation, so the fd table is the
       ONLY place they are visible;
    -  MON$MEMORY_USAGE at database level, each poll in a fresh
       transaction, because MON$ snapshots are per-transaction.

  Needs to run on the server machine with passwordless sudo (to read
  /proc/<serverpid>/fd of the firebird-owned process).
  See ../../sorting-and-temp-space.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/sorting && samples/fpc/bin/sorting
}
program sorting;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses {$IFDEF UNIX} cthreads, Unix, {$ENDIF} SysUtils, Classes, IB, FBHandsOn;

const DDL_TPB: array[0..3] of byte = (isc_tpb_write, isc_tpb_nowait,
        isc_tpb_read_committed, isc_tpb_rec_version);

const MON_SQL =
  'select m.mon$memory_allocated from mon$database d ' +
  'join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id';

var Running: boolean;                      { single writer: the main thread }
    PeakScratch, PeakFiles, PeakMem: int64;
    ServerPid: int64;

{ Sum the sizes of the server's open (already-unlinked) fb_sort_* files. }
procedure SampleScratch;
var f: TextFile;
    cmd, line: AnsiString;
    total, files, sz: int64;
begin
  cmd := Format('sudo -n find /proc/%d/fd -lname ''*fb_sort*'' -print0 2>/dev/null'
    + ' | xargs -0 -r sudo -n stat -L -c %%s 2>/dev/null', [ServerPid]);
  if POpen(f, cmd, 'R') <> 0 then
    exit;
  total := 0;
  files := 0;
  while not eof(f) do
  begin
    readln(f, line);
    if TryStrToInt64(Trim(line), sz) then
    begin
      total := total + sz;
      Inc(files);
    end;
  end;
  PClose(f);
  if total > PeakScratch then PeakScratch := total;
  if files > PeakFiles then PeakFiles := files;
end;

type
  TWatcher = class(TThread)
  protected
    procedure Execute; override;
  end;

procedure TWatcher.Execute;
var Mon: IAttachment;                       { second attachment: MON$ polling }
    Tr: ITransaction;
    mem: int64;
begin
  Mon := FirebirdAPI.OpenDatabase(DbConn('sorting'), DefaultDPB);
  while Running do
  begin
    SampleScratch;
    Tr := Mon.StartTransaction([isc_tpb_read, isc_tpb_nowait,
      isc_tpb_concurrency], taCommit);      { new tx => new MON$ snapshot }
    mem := Mon.OpenCursorAtStart(Tr, MON_SQL)[0].AsInt64;
    Tr.Commit;
    if mem > PeakMem then PeakMem := mem;
  end;
end;

type
  TCase = record
    lbl, sql: AnsiString;
  end;

const CASES: array[0..1] of TCase = (
  (lbl: 'big sort (200k rows, ~82 MB)';
   sql: 'select first 1 id from bulk order by pad desc'),
  (lbl: 'small sort (20k rows, ~8 MB)';
   sql: 'select first 1 id from bulk where mod(id, 10) = 0 order by pad desc'));

var Att: IAttachment;
    Tr: ITransaction;
    Stmt: IStatement;
    Cur: IResultSet;
    Watcher: TWatcher;
    memIdle, top: int64;
    c: integer;

begin
  Att := AttachOrCreate(DbConn('sorting'));

  Att.ExecImmediate(DDL_TPB,
    'recreate table bulk (id integer, pad varchar(400) character set ascii)');
  Att.ExecImmediate(DDL_TPB,
    'execute block as declare i integer = 0; begin' +
    '  while (i < 200000) do begin' +
    '    insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, ''x''));' +
    '    i = i + 1;' +
    '  end ' +
    'end');
  writeln('bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data');

  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  ServerPid := Att.OpenCursorAtStart(Tr,
    'select mon$server_pid from mon$attachments ' +
    'where mon$attachment_id = current_connection')[0].AsInt64;
  memIdle := Att.OpenCursorAtStart(Tr, MON_SQL)[0].AsInt64;
  Tr.Commit;
  writeln('server pid ', ServerPid, ', database memory allocated while idle: ',
    memIdle, ' bytes');

  for c := 0 to High(CASES) do
  begin
    PeakScratch := 0;
    PeakFiles := 0;
    PeakMem := 0;
    Running := true;
    Watcher := TWatcher.Create(false);

    Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait,
      isc_tpb_concurrency], taCommit);
    Stmt := Att.Prepare(Tr, CASES[c].sql);
    writeln;
    writeln(CASES[c].lbl);
    writeln('  ', Trim(Stmt.GetPlan));
    Cur := Stmt.OpenCursor;
    Cur.FetchNext;                          { the sort happens here }
    top := Cur[0].AsInt64;
    Cur.Close;
    Tr.Commit;

    Running := false;
    Watcher.WaitFor;
    Watcher.Free;
    writeln('  top row id = ', top);
    writeln('  peak fb_sort_* scratch: ', PeakFiles, ' file(s), ',
      PeakScratch, ' bytes');
    writeln('  peak database MON$MEMORY_ALLOCATED: ', PeakMem, ' bytes (+',
      PeakMem - memIdle, ' over idle)');
  end;

  writeln;
  writeln('done.');
end.
