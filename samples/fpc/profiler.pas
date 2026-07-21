{
  profiler.pas — the accumulation view, driven from client code.

  The fbintf twin of ../cpp/profiler.cpp: one profiler session brackets
  two workloads, then the PLG$PROFILER schema is queried like any other
  data — the document's argument that profiling output should be
  normalized tables, made runnable:

    - a join, read back from PLG$PROF_RECORD_SOURCE_STATS_VIEW as an
      indented plan tree with per-operator open/fetch counts and times;
    - a PSQL procedure with a hot loop, read back per line and column
      from PLG$PROF_PSQL_STATS_VIEW.

  Everything runs over a normal remote attachment: START_SESSION and
  FINISH_SESSION are just SQL, and the results are just rows fetched
  through the same IResultset as any other query.  See ../../profiler.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/profiler && samples/fpc/bin/profiler
}
program profiler;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;
    R: IResultset;
    profileId: AnsiString;

begin
  A := AttachOrCreate(DbConn('profiler'));
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);

  { --- workload fixtures: a table and a looping procedure --------------- }
  try A.ExecuteSQL(Tr, 'drop procedure hotspot', []);
  except on EIBInterBaseError do ; end;
  try A.ExecuteSQL(Tr, 'drop table nums', []);
  except on EIBInterBaseError do ; end;
  A.ExecuteSQL(Tr, 'create table nums (id int primary key, val int)', []);
  Tr.CommitRetaining;
  A.ExecuteSQL(Tr,
    'execute block as declare n int = 0; begin ' +
    '  while (n < 5000) do begin ' +
    '    insert into nums values (:n, mod(:n, 97)); n = n + 1; end end', []);
  A.ExecuteSQL(Tr,
    'create procedure hotspot returns (total bigint) as'#10 +
    '  declare i int = 0;'#10 +
    '  declare x int;'#10 +
    'begin'#10 +
    '  total = 0;'#10 +
    '  while (i < 20000) do'#10 +
    '  begin'#10 +
    '    select val from nums where id = mod(:i, 5000) into :x;'#10 +
    '    total = total + coalesce(:x, 0);'#10 +
    '    i = i + 1;'#10 +
    '  end'#10 +
    '  suspend;'#10 +
    'end', []);
  Tr.CommitRetaining;

  { --- profile: START_SESSION ... workload ... FINISH_SESSION ----------- }
  profileId := A.OpenCursorAtStart(Tr,
    'select rdb$profiler.start_session(''fpc hands-on'') from rdb$database')
    [0].AsString;

  A.OpenCursorAtStart(Tr,
    'select count(*) from nums a join nums b on b.id = a.val');
  A.OpenCursorAtStart(Tr, 'select total from hotspot');

  A.ExecuteSQL(Tr, 'execute procedure rdb$profiler.finish_session(true)', []);
  { The plugin flushes through an autonomous transaction; a retained
    SNAPSHOT would not see it, so start a genuinely new transaction. }
  Tr.Commit;
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('profile session ', profileId, ' finished and flushed');
  writeln;

  { --- the plan tree, with per-operator counters and times -------------- }
  writeln('record sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):');
  writeln(Format('%-70s %6s %8s %14s',
    ['ACCESS_PATH', 'OPENS', 'FETCHES', 'TOTAL_NS']));
  R := A.OpenCursorAtStart(Tr,
    'select cast(lpad('''', level * 2) || cast(access_path as varchar(120)) ' +
    '           as varchar(140)) as access_path, ' +
    '       open_counter as opens, fetch_counter as fetches, ' +
    '       open_fetch_total_elapsed_time as total_ns ' +
    'from plg$profiler.plg$prof_record_source_stats_view ' +
    'where profile_id = ' + profileId + ' ' +
    '  and sql_text containing ''join nums'' ' +
    'order by cursor_id, record_source_id');
  while not R.IsEof do
  begin
    writeln(Format('%-70s %6s %8s %14s',
      [R[0].AsString, R[1].AsString, R[2].AsString, R[3].AsString]));
    R.FetchNext;
  end;

  { --- the PSQL hotspot, per line and column ----------------------------- }
  writeln;
  writeln('hotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):');
  writeln(Format('%8s %10s %8s %14s %10s',
    ['LINE_NUM', 'COLUMN_NUM', 'COUNTER', 'TOTAL_NS', 'AVG_NS']));
  R := A.OpenCursorAtStart(Tr,
    'select line_num, column_num, counter, ' +
    '       total_elapsed_time as total_ns, avg_elapsed_time as avg_ns ' +
    'from plg$profiler.plg$prof_psql_stats_view ' +
    'where profile_id = ' + profileId + ' ' +
    '  and routine_name = ''HOTSPOT'' ' +
    'order by total_elapsed_time desc');
  while not R.IsEof do
  begin
    writeln(Format('%8s %10s %8s %14s %10s', [R[0].AsString, R[1].AsString,
      R[2].AsString, R[3].AsString, R[4].AsString]));
    R.FetchNext;
  end;

  Tr.Commit;
  writeln;
  writeln('done.');
end.
