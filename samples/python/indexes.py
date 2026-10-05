#
# indexes.py - one B-tree, many variants (Python twin of ../cpp/indexes.cpp;
# see ../../indexing-and-full-text-search.md).
#
# Builds a 3,000-row table with a descending, an expression (COMPUTED BY),
# a partial (WHERE) and a plain index, prepares five queries and prints the
# optimizer's plan for each: expression index, partial index, descending
# navigation, an OR bitmap-combining two indexes, and CONTAINING falling to
# NATURAL.  firebird-driver exposes BOTH plan forms as properties of the
# prepared Statement - stmt.plan is the legacy one-line PLAN the C++ sample
# prints (IStatement::getPlan(false)), stmt.detailed_plan the explained tree
# (getPlan(true)), shown here for the OR query.
#
# Run:  python3 indexes.py [database]
#
from fbsample import attach_or_create, db_path, execute, run, scalar


def plan(con, sql, detailed=False):
    cur = con.cursor()
    with cur.prepare(sql) as stmt:
        print(sql)
        print(stmt.plan.strip())
        if detailed:
            print('explained:')
            for line in stmt.detailed_plan.strip('\n').splitlines():
                print('  ' + line)
        print()
    cur.close()


def main():
    with attach_or_create(db_path('indexes')) as con:
        execute(con, 'recreate table doc ('
                     ' id integer, title varchar(60), status varchar(10), num integer)')
        execute(con, "execute block as declare i integer = 0; begin"
                     "  while (i < 3000) do begin"
                     "    insert into doc values (:i, 'Title ' || :i,"
                     "      iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100));"
                     "    i = i + 1;"
                     "  end "
                     "end")
        for ddl in ('create descending index doc_id_desc on doc (id)',
                    'create index doc_upper_title on doc computed by (upper(title))',
                    "create index doc_active on doc (status) where status = 'active'",
                    'create index doc_num on doc (num)'):
            execute(con, ddl, commit=False)
        con.commit()
        print('3000 rows; indexes: descending, expression, partial, plain\n')

        plan(con, "select id from doc where upper(title) = 'TITLE 5'")
        plan(con, "select id from doc where status = 'active'")
        plan(con, 'select first 1 id from doc order by id desc')
        plan(con, 'select id from doc where num = 42 or id = 7', detailed=True)
        plan(con, "select id from doc where title containing 'itle 12'")

        print('CONTAINING is correct but unindexed: matched',
              scalar(con, "select count(*) from doc where title containing 'itle 12'"),
              'rows by scanning all 3000')
        con.commit()
    print('done.')


if __name__ == '__main__':
    run(main)
