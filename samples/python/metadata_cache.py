#
# metadata_cache.py - the metadata cache's visibility rule from two
# attachments (Python twin of ../cpp/metadata_cache.cpp; see
# ../../metadata-cache.md).
#
#   1. an uncommitted ALTER is visible to its own transaction and to nobody
#      else - B gets "Column unknown" while A already selects the column;
#   2. once A commits, the new version is visible IMMEDIATELY, even to a
#      statement prepared inside B's older, still-open SNAPSHOT transaction:
#      metadata is read-committed, not snapshot-isolated;
#   3. two concurrent uncommitted DDLs on one object collide in
#      CacheElement::newVersion ("object in use", naming the MetaId and the
#      blocking transaction);
#   4. every ALTER appended a row to RDB$FORMATS; old records decode lazily.
#
# Every step runs in an explicit TransactionManager: which prepare happened
# in which transaction is the experiment, so nothing rides the connection's
# implicit main transaction.  B's SNAPSHOT is a typed TPB
# (Isolation.SNAPSHOT); errors are DatabaseError with the whole status
# vector (text, sqlcode, gds_codes).
#
# Run:  python3 metadata_cache.py [database]
#
from firebird.driver import TPB, DatabaseError, Isolation

from fbsample import (attach, attach_or_create, db_path, error_text, execute,
                      query, run)

SNAPSHOT = TPB(isolation=Isolation.SNAPSHOT).get_buffer()


def show(v):
    return '<null>' if v is None else str(v)


def try_query(who, con, tm, sql):
    """Print the first value of `sql`, or the error, on one line."""
    try:
        rows = query(con, sql, tr=tm)
        print(f'{who}: {sql} -> {show(rows[0][0])}')
    except DatabaseError as e:
        print(f'{who}: {sql} -> ERROR: {error_text(e).replace(chr(10), " ")}')


def begin(con, tpb=None):
    tm = con.transaction_manager(tpb)
    tm.begin()
    return tm


def main():
    path = db_path('mdc')
    with attach_or_create(path) as a, attach(path) as b:
        execute(a, 'recreate table t (a integer)')
        execute(a, 'insert into t values (1)')
        open_tms = []

        # -- 1. uncommitted DDL: mine, and mine alone ------------------------
        print('== 1. uncommitted ALTER: visible to creator only ==')
        a_ddl = begin(a)                         # A's DDL stays uncommitted
        execute(a, 'alter table t add e integer', tr=a_ddl)
        try_query('A (same tx)  ', a, a_ddl, 'select e from t')
        b_tra = begin(b)
        try_query('B            ', b, b_tra, 'select e from t')
        b_tra.commit()
        open_tms += [b_tra]

        # -- 2. committed DDL ignores open snapshots -------------------------
        print("\n== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==")
        b_snap = begin(b, SNAPSHOT)              # explicit SNAPSHOT
        try_query('B (snapshot) ', b, b_snap, 'select count(*) from t')
        a_ddl.commit()                           # E becomes committed
        t = begin(a)
        execute(a, 'alter table t add d integer', tr=t)
        t.commit()                               # D committed after B's snapshot
        try_query('B (same  tx) ', b, b_snap, 'select d from t')
        print('   (records are snapshot-isolated; metadata is read-committed -\n'
              "    the new statement was prepared against the chain's current head)")
        b_snap.commit()
        open_tms += [a_ddl, t, b_snap]

        # -- 3. concurrent DDL: the newVersion collision ---------------------
        print('\n== 3. two uncommitted DDLs on one object ==')
        a_ddl = begin(a)
        execute(a, 'alter table t add f integer', tr=a_ddl)
        b_tra = begin(b)
        try:
            execute(b, 'alter table t add g integer', tr=b_tra)
            print('B: ALTER unexpectedly succeeded')
        except DatabaseError as e:
            print('B: ALTER failed:')
            print(error_text(e))
            print(f'   (sqlcode {e.sqlcode}, gds_codes {list(e.gds_codes)})')
        b_tra.rollback()
        a_ddl.rollback()                         # F vanishes with the rollback
        open_tms += [a_ddl, b_tra]

        # -- 4. the on-disk half: one format per committed shape -------------
        print('\n== 4. RDB$FORMATS after the committed DDL ==')
        t = begin(a)
        n = query(a, "select count(*) from rdb$formats f "
                     "join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id "
                     "where r.rdb$relation_name = 'T'", tr=t)[0][0]
        print(f'formats stored for T: {n} (T has lived through that many shapes)')
        print('A E      D')
        for row in query(a, 'select a, e, d from t', tr=t):
            print(f'{row[0]} {show(row[1]):<6} {show(row[2])}')
        t.commit()
        open_tms.append(t)
        for tm in open_tms:
            tm.close()
    print('done.')


if __name__ == '__main__':
    run(main)
