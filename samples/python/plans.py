#
# plans.py - watch the cost-based optimizer decide, from the client (Python
# twin of ../cpp/plans.cpp; see ../../query-optimizer-and-execution.md).
#
# Every statement is prepared (never executed!) and both plan forms are
# printed:
#   - legacy:   PLAN (EMP INDEX (EMP_DEPT))          - the terse tree
#   - detailed: -> Table ... Access By ID -> Bitmap  - the record-source tree
# The same SELECT is prepared before and after CREATE INDEX, so the flip
# from a Full Scan to a Bitmap + Index Range Scan is visible; a join with
# ORDER BY shows SORT over a nested loop, and an indexless equi-join shows
# the Firebird 5 hash join.
#
# firebird-driver exposes IStatement::getPlan as two Statement properties,
# .plan (legacy) and .detailed_plan, on the object Cursor.prepare() returns -
# the C++ route, both forms, no RDB$SQL.EXPLAIN detour.
#
# Run:  python3 plans.py [database]
#
from fbsample import attach_or_create, db_path, execute, run


def show_plan(cur, sql):
    print('==', sql)
    with cur.prepare(sql) as stmt:          # prepared only, never executed
        print('legacy: ', stmt.plan)
        print('detailed:')
        print(stmt.detailed_plan)
    print()


def main():
    with attach_or_create(db_path('plans')) as db:
        # -- Build the schema: 20 departments, 2000 employees. -------------
        execute(db, 'RECREATE TABLE dept (id INT NOT NULL PRIMARY KEY, name VARCHAR(20))')
        execute(db, 'RECREATE TABLE emp (id INT NOT NULL PRIMARY KEY,'
                    ' dept_id INT, salary INT, name VARCHAR(20))')
        execute(db, '''EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN
  WHILE (i <= 20) DO BEGIN
    INSERT INTO dept VALUES (:i, 'dept ' || :i); i = i + 1;
  END
  i = 1;
  WHILE (i <= 2000) DO BEGIN
    INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,
        1000 + MOD(:i * 37, 500), 'emp ' || :i); i = i + 1;
  END
END''')
        cur = db.cursor()

        # -- 1. No index on dept_id yet: the full scan is the only path. ---
        show_plan(cur, 'SELECT name FROM emp WHERE dept_id = 5')

        # -- 2. Create the index; the same text now compiles differently. --
        execute(db, 'CREATE INDEX emp_dept ON emp (dept_id)')
        print('-- CREATE INDEX emp_dept ON emp (dept_id) --\n')
        show_plan(cur, 'SELECT name FROM emp WHERE dept_id = 5')

        # -- 3. PK equality: unique index, nothing cheaper than one row. ---
        show_plan(cur, 'SELECT name FROM emp WHERE id = 42')

        # -- 4. Join + ORDER BY: SORT over a nested loop with the index. ---
        show_plan(cur, 'SELECT e.name, d.name FROM emp e'
                       ' JOIN dept d ON e.dept_id = d.id ORDER BY e.salary')

        # -- 5. Equi-join with no usable index on either side: hash join. --
        show_plan(cur, 'SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary')
        cur.close()
    print('done.')


if __name__ == '__main__':
    run(main)
