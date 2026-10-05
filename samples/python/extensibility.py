#
# extensibility.py - calling native code through SQL: UDR end to end
# (Python twin of ../cpp/extensibility.cpp; see ../../extensibility.md).
#
# The shipped example UDR module (plugins/udr/libudrcpp_example.so) is bound
# to SQL names with EXTERNAL NAME '<module>!<entry>' ENGINE udr and called
# like any other procedure/function; then RDB$PROCEDURES / RDB$FUNCTIONS show
# the binding as metadata and RDB$CONFIG names the plugin filling each role.
# Every seam here lives on the server, so the driver loses nothing: the DDL
# is cursor.execute, the selectable procedure is a plain fetch loop, and the
# listing headers come from cursor.description, the driver's view of the
# statement's output metadata.
#
# Run:  python3 extensibility.py [database]
#
from fbsample import attach_or_create, db_path, execute, query, run, scalar


def table(con, sql):
    """isql-style listing: column names from cursor.description."""
    cur = con.cursor()
    cur.execute(sql)
    rows = [['' if v is None else str(v).strip() for v in r] for r in cur.fetchall()]
    names = [d[0] for d in cur.description]
    cur.close()
    widths = [max([len(n)] + [len(r[i]) for r in rows]) for i, n in enumerate(names)]
    print(' '.join(n.ljust(w) for n, w in zip(names, widths)).rstrip())
    print(' '.join('-' * w for w in widths))
    for r in rows:
        print(' '.join(v.ljust(w) for v, w in zip(r, widths)).rstrip())


def main():
    with attach_or_create(db_path('extensibility')) as con:
        # 1. Bind SQL names to entry points in the shipped native module.
        execute(con, "recreate procedure gen_rows (start_n integer not null, "
                     "                             end_n integer not null) "
                     "  returns (n integer not null) "
                     "  external name 'udrcpp_example!gen_rows' engine udr")
        execute(con, "recreate function sum_args (n1 integer, n2 integer, n3 integer) "
                     "  returns integer "
                     "  external name 'udrcpp_example!sum_args' engine udr")

        # 2. Call them: native C++ running inside the server, plain SQL here.
        print('select n from gen_rows(1, 5):')
        table(con, 'select n from gen_rows(1, 5)')
        print('\nselect sum_args(19, 20, 3): ',
              scalar(con, 'select sum_args(19, 20, 3) from rdb$database'))

        # 3. The binding is ordinary metadata...
        print('\nexternal routines recorded in the system tables:')
        for (line,) in query(con,
                "select trim(rdb$procedure_name) || '  ->  ' || "
                "       trim(rdb$entrypoint) || '  (engine ' || "
                "       trim(rdb$engine_name) || ')' "
                "from rdb$procedures where rdb$engine_name = 'UDR' "
                "union all "
                "select trim(rdb$function_name) || '  ->  ' || "
                "       trim(rdb$entrypoint) || '  (engine ' || "
                "       trim(rdb$engine_name) || ')' "
                "from rdb$functions where rdb$engine_name = 'UDR'"):
            print(line)

        # 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG.
        print('\nplugins filling each role (rdb$config):')
        table(con, "select rdb$config_name, rdb$config_value "
                   "from rdb$config "
                   "where rdb$config_name in ('Providers', 'AuthServer', "
                   "      'UserManager', 'WireCryptPlugin', 'TracePlugin', "
                   "      'DefaultProfilerPlugin') "
                   "order by rdb$config_id")
        con.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
