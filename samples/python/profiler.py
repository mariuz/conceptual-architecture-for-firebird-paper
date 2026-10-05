#
# profiler.py - the profiler's accumulation view, driven from client code
# (Python twin of ../cpp/profiler.cpp; see ../../profiler.md).
#
# One RDB$PROFILER session brackets two workloads - a self-join over a
# 5,000-row table and a 20,000-iteration PSQL loop - then the PLG$PROFILER
# schema is queried like any other data: the record-source view as an
# indented plan tree, the PSQL view ranked by time per line and column.
#
# The control surface is a SQL package and the output a SQL schema, so the
# driver needs nothing special.  The one pitfall is the same as in every
# explicit-transaction twin: the plugin flushes through an autonomous
# transaction, and firebird-driver's connection keeps a SNAPSHOT main
# transaction - Connection.commit() must really end it (commit_retaining
# would keep the old snapshot) before the views are read.
#
# Run:  python3 profiler.py [database]
#
from firebird.driver import DatabaseError

from fbsample import attach_or_create, db_path, execute, query, run, scalar

HOTSPOT = '''create procedure hotspot returns (total bigint) as
  declare i int = 0;
  declare x int;
begin
  total = 0;
  while (i < 20000) do
  begin
    select val from nums where id = mod(:i, 5000) into :x;
    total = total + coalesce(:x, 0);
    i = i + 1;
  end
  suspend;
end'''


def table(rows, headers):
    """Print rows under headers, columns sized to fit."""
    cells = [headers] + [['' if v is None else str(v) for v in r] for r in rows]
    widths = [max(len(c[i]) for c in cells) for i in range(len(headers))]
    for n, line in enumerate(cells):
        print(' '.join(v.ljust(w) for v, w in zip(line, widths)).rstrip())
        if n == 0:
            print(' '.join('-' * w for w in widths))


def main():
    with attach_or_create(db_path('profiler')) as db:
        # --- workload fixtures: a table and a looping procedure ------------
        for ddl in ('drop procedure hotspot', 'drop table nums'):
            try:
                execute(db, ddl)
            except DatabaseError:
                db.rollback()
        execute(db, 'create table nums (id int primary key, val int)')
        execute(db, 'execute block as declare n int = 0; begin '
                    '  while (n < 5000) do begin '
                    '    insert into nums values (:n, mod(:n, 97)); n = n + 1; end end')
        execute(db, HOTSPOT)

        # --- profile: START_SESSION ... workload ... FINISH_SESSION --------
        profile_id = scalar(db, "select rdb$profiler.start_session('python hands-on') "
                                'from rdb$database')
        scalar(db, 'select count(*) from nums a join nums b on b.id = a.val')
        scalar(db, 'select total from hotspot')
        execute(db, 'execute procedure rdb$profiler.finish_session(true)')
        # execute() ended with db.commit(): a hard commit, so the next
        # statement starts a fresh snapshot that sees the autonomous flush.
        print(f'profile session {profile_id} finished and flushed\n')

        # --- the plan tree, with per-operator counters and times -----------
        print('record sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):')
        table(query(db,
            "select cast(lpad('', level * 2) || cast(access_path as varchar(120)) "
            '            as varchar(140)), '
            '       open_counter, fetch_counter, open_fetch_total_elapsed_time '
            'from plg$profiler.plg$prof_record_source_stats_view '
            "where profile_id = ? and sql_text containing 'join nums' "
            'order by cursor_id, record_source_id', (profile_id,)),
            ['ACCESS_PATH', 'OPENS', 'FETCHES', 'TOTAL_NS'])

        # --- the PSQL hotspot, per line and column -------------------------
        print('\nhotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):')
        table(query(db,
            'select line_num, column_num, counter, total_elapsed_time, avg_elapsed_time '
            'from plg$profiler.plg$prof_psql_stats_view '
            "where profile_id = ? and routine_name = 'HOTSPOT' "
            'order by total_elapsed_time desc', (profile_id,)),
            ['LINE_NUM', 'COLUMN_NUM', 'COUNTER', 'TOTAL_NS', 'AVG_NS'])
        db.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
