#
# stmt_cache.py - the server's DSQL statement cache inferred from prepare
# timings (Python twin of ../cpp/stmt_cache.cpp; see ../../statement-cache.md).
#
# There is no monitoring view of the cache, so - like the document's own
# demonstrations - the sample times prepares of a statement that is heavy
# to *compile* (a six-way self-join: large join-order search) and is never
# executed; every loop is prepare + free only:
#
#   run 1  identical text              -> all but the first prepare hit
#   run 2  same SQL + i trailing spaces-> all miss: the key is the text verbatim
#   run 3  distinct literal each time  -> all miss, for comparison
#   run 4  identical text, each prepare preceded by an unrelated
#          RECREATE TABLE + commit     -> all miss: a DDL commit purges the cache
#
# firebird-driver, like fbintf, keeps no client-side statement cache and
# can prepare without executing: Cursor.prepare() returns a Statement that
# Statement.free() releases, so the timings reach the server cache with
# nothing in the way.  (The driver's prepare also requests the statement
# metadata, as the C++ twins' prepares do.)
#
# Run:  python3 stmt_cache.py [database]
#
import time

from fbsample import attach_or_create, db_path, execute, run

HEAVY = ('SELECT COUNT(*) FROM t a'
         ' JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id'
         ' JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id'
         ' JOIN t f ON e.id = f.id WHERE a.id > 0')
N = 100


def prepare_once(cur, sql):
    """Prepare and free; returns the elapsed milliseconds."""
    t0 = time.perf_counter()
    cur.prepare(sql).free()
    return (time.perf_counter() - t0) * 1000


def report(n, label, total, verdict):
    print(f'{n}. {label:<26}{N:4d} prepares: {total:6.1f} ms  '
          f'({total / N:.2f} ms/prepare) - {verdict}')


def main():
    with attach_or_create(db_path('stmt_cache')) as db:
        execute(db, 'RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)')
        execute(db, 'EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO'
                    ' BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END')

        tra = db.transaction_manager()
        tra.begin()
        cur = tra.cursor()
        prepare_once(cur, HEAVY)                     # warm the cache

        report(1, 'identical text', sum(prepare_once(cur, HEAVY) for _ in range(N)), 'hits')
        report(2, '+ i trailing spaces',
               sum(prepare_once(cur, HEAVY + ' ' * (i + 1)) for i in range(N)), 'misses')
        report(3, 'distinct literal',
               sum(prepare_once(cur, HEAVY.replace('> 0', f'> {i}')) for i in range(N)),
               'misses')

        total = 0.0                                  # time only the prepares
        for _ in range(N):
            execute(db, 'RECREATE TABLE unrelated (x INT)')   # commit purges
            total += prepare_once(cur, HEAVY)
        report(4, 'identical text after DDL', total, 'misses')

        cur.close()
        tra.commit()
        tra.close()


if __name__ == '__main__':
    run(main)
