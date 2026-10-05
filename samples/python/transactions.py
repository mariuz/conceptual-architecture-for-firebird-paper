#
# transactions.py - MVCC isolation seen from the client (Python twin of
# ../cpp/transactions_demo.cpp; see ../../transactions-and-concurrency.md).
#
# Two attachments play the same scenario: SNAPSHOT stability, READ
# COMMITTED freshness, then a NO WAIT write conflict.  firebird-driver takes
# the TPB as a typed TPB object (Isolation.SNAPSHOT, Isolation.READ_COMMITTED_
# RECORD_VERSION, lock_timeout=0 for NO WAIT) and encodes the same
# isc_tpb_* bytes the C++ sample packs by hand.
#
# Run:  python3 transactions.py [database]
#
from firebird.driver import TPB, DatabaseError, Isolation

from fbsample import (attach, attach_or_create, db_path, error_text, execute,
                      run, scalar)

SNAPSHOT = TPB(isolation=Isolation.SNAPSHOT, lock_timeout=0).get_buffer()
READ_COMMITTED = TPB(isolation=Isolation.READ_COMMITTED_RECORD_VERSION,
                     lock_timeout=0).get_buffer()

SELECT = 'select amount from balance where id = 1'


def main():
    path = db_path('tx')
    with attach_or_create(path) as a, attach(path) as b:
        execute(a, 'recreate table balance (id integer primary key, amount integer)')
        execute(a, 'insert into balance values (1, 100)')

        # --- 1. SNAPSHOT stability ------------------------------------------
        snap_a = a.transaction_manager(SNAPSHOT)
        snap_a.begin()
        print('A (SNAPSHOT)       sees amount =', scalar(a, SELECT, tr=snap_a))

        execute(b, 'update balance set amount = 999 where id = 1')
        print('B                  committed amount = 999')

        print('A (same SNAPSHOT)  sees amount =', scalar(a, SELECT, tr=snap_a),
              '  <- still the start-of-tx version')

        # --- 2. READ COMMITTED sees the new version ---------------------------
        rc_a = a.transaction_manager(READ_COMMITTED)
        rc_a.begin()
        print('A (READ COMMITTED) sees amount =', scalar(a, SELECT, tr=rc_a),
              '  <- the committed version')
        rc_a.commit()

        # --- 3. Write conflict under NO WAIT ----------------------------------
        hold_b = b.transaction_manager(SNAPSHOT)
        hold_b.begin()
        execute(b, 'update balance set amount = amount + 1 where id = 1', tr=hold_b)

        loser_a = a.transaction_manager(SNAPSHOT)
        loser_a.begin()
        try:
            execute(a, 'update balance set amount = amount + 10 where id = 1',
                    tr=loser_a)
            print('unexpected: conflicting update succeeded')
        except DatabaseError as e:
            print('A conflicting update failed as designed:')
            print('    ' + error_text(e).replace('\n', '\n    '))
            print('    sqlcode', e.sqlcode, '/ gds', e.gds_codes[0] if e.gds_codes else '?')

        loser_a.rollback()
        hold_b.commit()
        snap_a.commit()
        for tm in (snap_a, rc_a, hold_b, loser_a):
            tm.close()
    print('done.')


if __name__ == '__main__':
    run(main)
