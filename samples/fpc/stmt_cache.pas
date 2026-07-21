{
  stmt_cache.pas — the server's DSQL statement cache, timed from fbintf.

  The fbintf twin of ../cpp/stmt_cache.cpp.  fbintf keeps NO client-side
  cache of prepared statements — IAttachment.Prepare always sends a real
  dsql prepare to the server (grep extern/fbintf/client for a statement
  cache: there is none; every Prepare builds a fresh TFB30Statement).
  That makes prepare timings a clean probe of the SERVER-side cache,
  exactly as in the C++ twin: a statement that is heavy to *compile*
  (six-way self-join: large join-order search) and never executed.

    run 1  identical text              -> all but the first prepare hit
    run 2  same SQL + i trailing spaces-> all miss: the key is the text
           verbatim, whitespace included (buildStatementKey)
    run 3  distinct literal each time  -> all miss, for comparison
    run 4  identical text, but each prepare preceded by an unrelated
           RECREATE TABLE + commit     -> all miss: any DDL commit purges
           the whole cache (dfw.epp -> purgeAllAttachments)

  See ../../statement-cache.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/stmt_cache && samples/fpc/bin/stmt_cache
}
program stmt_cache;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const HEAVY =
  'SELECT COUNT(*) FROM t a' +
  ' JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id' +
  ' JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id' +
  ' JOIN t f ON e.id = f.id WHERE a.id > 0';

const N = 100;

const DDL_TPB: array[0..3] of byte = (isc_tpb_write, isc_tpb_nowait,
        isc_tpb_read_committed, isc_tpb_rec_version);

var Att: IAttachment;
    Tr: ITransaction;
    i: integer;
    t0: QWord;
    t: double;

procedure PrepareOnce(const sql: AnsiString);
var Stmt: IStatement;
begin
  Stmt := Att.Prepare(Tr, sql);   { a real wire round trip, every time }
  Assert(Stmt.IsPrepared);        { scope exit -> refcount drop -> DSQL free }
end;

procedure Report(const label_: AnsiString; ms: double);
begin
  writeln(Format('%s %3d prepares: %8.1f ms  (%.2f ms/prepare)',
    [label_, N, ms, ms / N]));
end;

begin
  Att := AttachOrCreate(DbConn('stmt_cache'));

  Att.ExecImmediate(DDL_TPB, 'RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)');
  Att.ExecImmediate(DDL_TPB,
    'EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO' +
    ' BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END');

  writeln('fbintf has no client-side statement cache: every Prepare is a');
  writeln('server round trip, so these timings probe the server DSQL cache.');
  writeln;

  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  PrepareOnce(HEAVY);   { warm the cache with the exact text }

  t0 := GetTickCount64;
  for i := 0 to N - 1 do
    PrepareOnce(HEAVY);
  Report('1. identical text           - hits  ', GetTickCount64 - t0);

  t0 := GetTickCount64;
  for i := 0 to N - 1 do
    PrepareOnce(HEAVY + StringOfChar(' ', i + 1));
  Report('2. + i trailing spaces      - misses', GetTickCount64 - t0);

  t0 := GetTickCount64;
  for i := 0 to N - 1 do
    PrepareOnce(StringReplace(HEAVY, '> 0', '> ' + IntToStr(i), []));
  Report('3. distinct literal         - misses', GetTickCount64 - t0);

  t := 0;   { time only the prepares, not the DDL itself }
  for i := 0 to N - 1 do
  begin
    { own transaction, committed at once: purges every cache in the db }
    Att.ExecImmediate(DDL_TPB, 'RECREATE TABLE unrelated (x INT)');
    t0 := GetTickCount64;
    PrepareOnce(HEAVY);
    t := t + (GetTickCount64 - t0);
  end;
  Report('4. identical text after DDL - misses', t);

  Tr.Commit;
  writeln;
  writeln('done.');
end.
