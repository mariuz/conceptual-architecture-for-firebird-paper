#
# windows.py - window functions and analytic aggregates live (Python twin of
# ../cpp/windows.cpp; see ../../aggregate-and-window-functions.md).
#
# Recreates the document's six-row sales table and runs its flagship
# analytics: the ranking / framed running total / LAG window query, FILTER +
# LISTAGG + STDDEV_POP, PERCENTILE_CONT with a hypothetical-set RANK(175)
# WITHIN GROUP, and Firebird 6's EXCLUDE CURRENT ROW frame.
#
# The plan comes from the driver's own statement object: Cursor.prepare()
# returns a Statement whose .plan is the legacy plan and .detailed_plan the
# explained one (both isc_info_sql_*plan info items), so no RDB$SQL.EXPLAIN
# detour is needed.  Values arrive typed: NUMERIC as decimal.Decimal, the
# median as float, LAG's first-row NULL as None - and the running total's
# SUM, widened to NUMERIC(20,2) and so travelling as INT128, decodes to
# Decimal with no CAST (the Rust twin has to cast it back to NUMERIC(10,2)).
#
# Run:  python3 windows.py [database]
#
from fbsample import attach_or_create, db_path, execute, run

WINDOW_SQL = """
SELECT region, amount,
  ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount) AS rn,
  RANK() OVER (ORDER BY amount DESC) AS overall_rank,
  SUM(amount) OVER (PARTITION BY region ORDER BY id
    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_total,
  LAG(amount) OVER (PARTITION BY region ORDER BY id) AS prev_amount
FROM sales"""


def show(cur, sql):
    """Execute and print an isql-style table headed by the column aliases."""
    cur.execute(sql)
    names = [d[0] for d in cur.description]
    rows = [['<null>' if v is None else str(v) for v in r] for r in cur.fetchall()]
    widths = [max(len(n), *(len(r[i]) for r in rows)) for i, n in enumerate(names)]
    print(' '.join(n.ljust(w) for n, w in zip(names, widths)))
    print(' '.join('-' * w for w in widths))
    for r in rows:
        print(' '.join(v.ljust(w) for v, w in zip(r, widths)).rstrip())


def main():
    with attach_or_create(db_path('windows')) as con:
        execute(con, 'RECREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(10),'
                     ' amount NUMERIC(10,2))')
        cur = con.cursor()
        cur.executemany('INSERT INTO sales VALUES (?, ?, ?)',
                        [(1, 'East', 100), (2, 'East', 200), (3, 'East', 150),
                         (4, 'West', 300), (5, 'West', 250), (6, 'West', 400)])

        # -- 1. partitioned ranking, framed running total, LAG navigation
        print('== window functions ==')
        show(cur, WINDOW_SQL)
        stmt = cur.prepare(WINDOW_SQL)
        print('\nplan:' + stmt.plan)
        print('\ndetailed plan:' + stmt.detailed_plan)
        stmt.free()

        # -- 2. FILTER (FB5), ordered LISTAGG, statistical aggregate
        print('\n== aggregates: FILTER / LISTAGG / STDDEV_POP ==')
        show(cur, "SELECT region, COUNT(*) AS n,"
                  " COUNT(*) FILTER (WHERE amount > 150) AS big_sales,"
                  " CAST(LISTAGG(amount, ',') WITHIN GROUP (ORDER BY amount)"
                  "   AS VARCHAR(60)) AS amounts,"
                  " CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev"
                  " FROM sales GROUP BY region")

        # -- 3. ordered-set median and hypothetical-set rank
        print('\n== PERCENTILE_CONT median / hypothetical RANK(175) ==')
        show(cur, "SELECT region,"
                  " PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median,"
                  " RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175"
                  " FROM sales GROUP BY region")

        # -- 4. FB6 frame exclusion: the neighbours' average, row left out
        print("\n== FB6 frame EXCLUDE CURRENT ROW (neighbours' average) ==")
        show(cur, "SELECT id, amount,"
                  " CAST(AVG(amount) OVER (ORDER BY id"
                  "   ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING"
                  "   EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg"
                  " FROM sales")
        cur.close()
        con.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
