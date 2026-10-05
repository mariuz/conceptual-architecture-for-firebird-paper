#
# lock_manager.py - drive the real lock manager and time all three wait
# outcomes (Python twin of ../cpp/lock_manager.cpp; see
# ../../lock-manager.md).
#
# A reservation FOR PROTECTED WRITE takes a genuine LCK_relation lock at
# LCK_EX, so the probes exercise enqueue / grant_or_que / wait_for_request,
# not the MVCC record-conflict path:
#
#     NO WAIT        -> isc_lock_conflict, immediately   (lck_wait == 0)
#     LOCK TIMEOUT 3 -> isc_lock_timeout, after ~3 s     (lck_wait < 0)
#     WAIT           -> granted the moment the holder commits (lck_wait > 0)
#
# firebird-driver expresses the reservation in the TPB itself, like the
# Free Pascal twin and unlike the C++ ones (which go through SQL):
# TPB.reserve_table('T1', TableShareMode.PROTECTED, TableAccessMode.LOCK_WRITE)
# encodes isc_tpb_lock_write "T1" + isc_tpb_protected, and lock_timeout
# (0 / 3 / -1) picks isc_tpb_nowait / isc_tpb_lock_timeout / isc_tpb_wait.
#
# A final act builds a real deadlock through LCK_tra transaction locks and
# measures how long the periodic scanner (DeadlockTimeout = 10 s) takes.
#
# Run:  python3 lock_manager.py [database]
#
import threading
import time

from firebird.driver import (TPB, DatabaseError, TableAccessMode,
                             TableShareMode)

from fbsample import attach, attach_or_create, db_path, execute, run


def reserving(lock_timeout):
    """A TPB reserving T1 FOR PROTECTED WRITE with the given lock_timeout."""
    tpb = TPB(lock_timeout=lock_timeout)
    tpb.reserve_table('T1', TableShareMode.PROTECTED, TableAccessMode.LOCK_WRITE)
    return tpb.get_buffer()


def first_line(exc):
    return str(exc).splitlines()[0].strip()


def probe(con, label, tpb):
    tm = con.transaction_manager(tpb)
    t0 = time.monotonic()
    try:
        tm.begin()           # the reservation is taken at transaction start
        print(f'{label:<16} granted after {time.monotonic() - t0:.3f} s')
        tm.commit()
    except DatabaseError as e:
        print(f'{label:<16} failed after {time.monotonic() - t0:.3f} s: {first_line(e)}')
    finally:
        tm.close()


def main():
    path = db_path('lock_manager')
    with attach_or_create(path) as a, attach(path) as b:
        execute(a, 'recreate table t1 (id int primary key, v int)')
        execute(a, 'insert into t1 values (1, 0)')
        execute(a, 'insert into t1 values (2, 0)')

        # A holds the LCK_relation lock at EX for the whole first act.
        hold = a.transaction_manager(reserving(-1))
        hold.begin()
        print('holder: t1 reserved FOR PROTECTED WRITE (LCK_relation at LCK_EX)')

        probe(b, 'NO WAIT:', reserving(0))
        probe(b, 'LOCK TIMEOUT 3:', reserving(3))

        # WAIT parks in wait_for_request until the holder lets go: release
        # the reservation from another thread after 2 s.
        def release():
            time.sleep(2)
            hold.commit()
            print('holder: committed (2 s later) -> lock released')
        releaser = threading.Thread(target=release)
        releaser.start()
        probe(b, 'WAIT:', reserving(-1))
        releaser.join()
        hold.close()

        # Act two: a genuine wait-for cycle through LCK_tra locks.  Both
        # sides block in WAIT mode; nobody looks for the cycle until the
        # periodic scan fires - expect ~DeadlockTimeout seconds, not ~0.
        print('building deadlock: A updates row 1, B updates row 2, then cross...')
        ta = a.transaction_manager()   # empty TPB: engine default SNAPSHOT WAIT
        tb = b.transaction_manager()
        ta.begin()
        tb.begin()
        execute(a, 'update t1 set v = v + 1 where id = 1', tr=ta)
        execute(b, 'update t1 set v = v + 1 where id = 2', tr=tb)

        t0 = time.monotonic()
        victim = {}

        def cross_a():
            try:
                execute(a, 'update t1 set v = v + 1 where id = 2', tr=ta)
            except DatabaseError as e:      # this side was chosen as victim
                print(f'deadlock: A failed after {time.monotonic() - t0:.1f} s: '
                      f'{first_line(e)}')
                victim['a'] = True
                ta.rollback()               # free B
        crosser = threading.Thread(target=cross_a)
        crosser.start()
        time.sleep(0.3)
        try:
            execute(b, 'update t1 set v = v + 1 where id = 1', tr=tb)
            print(f"deadlock: B's update proceeded after {time.monotonic() - t0:.1f} s "
                  '(A was the victim)')
        except DatabaseError as e:
            print(f'deadlock: B failed after {time.monotonic() - t0:.1f} s: {first_line(e)}')
            victim['b'] = True
            tb.rollback()                   # free A
        crosser.join()
        if 'a' not in victim:
            ta.rollback()
        if 'b' not in victim:
            tb.rollback()
        ta.close()
        tb.close()
        print('the wait is DeadlockTimeout (10 s default): the cycle sat '
              'undetected until the scan.')


if __name__ == '__main__':
    run(main)
