#
# request_lifecycle.py - one CREATE TABLE round trip, instrumented from the
# client (Python twin of ../cpp/request_lifecycle.cpp; see
# ../../request-lifecycle-code-trace.md).
#
# Each API step is timed, and the attachment's own MON$IO_STATS /
# MON$RECORD_STATS counters are sampled around them:
#
#   prepare  -> DSQL: parser picks DsqlDdlStatement (Statement.type == DDL)
#   execute  -> EXE/MET: STORE into RDB$RELATIONS etc. - record inserts jump
#               (VIO_store); the new row is visible to *this* transaction
#   commit   -> TRA_commit -> DFW_perform_work -> CCH_flush -> PIO_write -
#               the page-write counter jumps
#
# firebird-driver keeps the three phases separate - Cursor.prepare() returns
# a Statement, Cursor.execute(stmt) runs it, TransactionManager.commit()
# ends it - and a Connection carries any number of TransactionManagers, so
# each MON$ sample is a fresh transaction on the same attachment (MON$
# snapshots are frozen per transaction), and a second one checks the
# uncommitted catalog row from outside.
#
# Run:  python3 request_lifecycle.py [database]
#
import time

from firebird.driver import DatabaseError, StatementType

from fbsample import attach_or_create, db_path, execute, run, scalar

STATS = ('SELECT i.MON$PAGE_FETCHES, i.MON$PAGE_MARKS, i.MON$PAGE_WRITES,'
         '       r.MON$RECORD_INSERTS'
         ' FROM MON$ATTACHMENTS a'
         ' JOIN MON$IO_STATS i ON a.MON$STAT_ID = i.MON$STAT_ID'
         ' JOIN MON$RECORD_STATS r ON a.MON$STAT_ID = r.MON$STAT_ID'
         ' WHERE a.MON$ATTACHMENT_ID = CURRENT_CONNECTION')
SEEN = "SELECT COUNT(*) FROM RDB$RELATIONS WHERE RDB$RELATION_NAME = 'TRACE_DEMO'"


def in_fresh_tx(con, fn):
    """Run fn(tm) in a brand-new transaction on `con`, then commit it."""
    with con.transaction_manager() as tm:
        tm.begin()
        try:
            return fn(tm)
        finally:
            tm.commit()


def sample(con):
    def read(tm):
        with tm.cursor() as cur:
            cur.execute(STATS)
            return dict(zip(('fetches', 'marks', 'writes', 'ins'), cur.fetchone()))
    return in_fresh_tx(con, read)


def ms_since(t0):
    return (time.perf_counter() - t0) * 1000


def main():
    with attach_or_create(db_path('request_lifecycle')) as db:
        try:                                  # idempotency
            execute(db, 'DROP TABLE trace_demo')
        except DatabaseError:
            db.rollback()

        s0 = sample(db)
        tra = db.transaction_manager()
        tra.begin()
        cur = tra.cursor()

        # -- prepare: Y-valve -> remote -> DSQL (Stages 1-5) ----------------
        t0 = time.perf_counter()
        stmt = cur.prepare('CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY,'
                           ' name VARCHAR(30))')
        print(f'prepare  {ms_since(t0):6.2f} ms   statement type = {stmt.type.name}'
              + ('' if stmt.type == StatementType.DDL else '  (?)'))

        # -- execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) -----
        t0 = time.perf_counter()
        cur.execute(stmt)
        t_exec = ms_since(t0)
        s1 = sample(db)
        print(f'execute  {t_exec:6.2f} ms   catalog record inserts: +{s1["ins"] - s0["ins"]}, '
              f'page marks: +{s1["marks"] - s0["marks"]}')
        print('         in this tx:  RDB$RELATIONS has TRACE_DEMO =', scalar(db, SEEN, tr=tra))
        print('         other tx:    RDB$RELATIONS has TRACE_DEMO =',
              in_fresh_tx(db, lambda tm: scalar(db, SEEN, tr=tm)),
              ' (TRA_commit has not happened)')

        # -- commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9) --
        stmt.free()
        cur.close()
        t0 = time.perf_counter()
        tra.commit()
        t_commit = ms_since(t0)
        s2 = sample(db)
        print(f'commit   {t_commit:6.2f} ms   page writes: +{s2["writes"] - s1["writes"]}  '
              f'(fetches: +{s2["fetches"] - s0["fetches"]} over the whole trip)')
        print('         other tx:    RDB$RELATIONS has TRACE_DEMO =',
              in_fresh_tx(db, lambda tm: scalar(db, SEEN, tr=tm)))
        tra.close()
    print('done.')


if __name__ == '__main__':
    run(main)
