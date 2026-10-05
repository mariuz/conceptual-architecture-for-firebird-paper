#
# gc_sweep.py - the record-version lifecycle seen through MON$RECORD_STATS
# (Python twin of ../cpp/gc_sweep.cpp; see ../../garbage-collection-and-sweep.md).
#
# Pins a SNAPSHOT, commits twelve updates under it, releases it, scans, then
# deletes - watching the database-level counters that are vio.cpp's events
# (MON$RECORD_IMGC = VIO_intermediate_gc, PURGES = purge(), EXPUNGES =
# expunge(), MON$BACKVERSION_READS = chain walks).  Then a no_auto_undo
# rollback leaves a stump that freezes the OIT.
#
# firebird-driver reaches every lever the C++ sample packs by hand: the pin
# and the stump are typed TPBs (Isolation.SNAPSHOT; no_auto_undo=True), the
# four header counters are info calls on the attachment (con.info.oit / oat
# / ost / next_transaction) rather than a MON$DATABASE query, and the sweep
# the C++ sample only recommends is one Services call here -
# Server.database.sweep() - after which the OIT is seen to move past the
# stump.  One driver trap, worth knowing: connect_server(server) looks
# `server` up in driver_config and, for an unregistered name such as
# 'localhost', silently falls back to the defaults - host None, i.e. a
# LOCAL (embedded) service manager, whose engine then collides with the
# server's exclusive lock ("Database already opened with engine instance,
# incompatible with current").  fbsample.server() registers the host
# first, so the sweep really runs inside the server.
#
# Run:  python3 gc_sweep.py [database]
#
import time

from firebird.driver import TPB, Isolation

from fbsample import attach, db_path, execute, recreate, run, scalar, server

SNAPSHOT = TPB(isolation=Isolation.SNAPSHOT).get_buffer()
STUMP = TPB(isolation=Isolation.SNAPSHOT, no_auto_undo=True).get_buffer()
VAL = 'select val from gctest where id = 1'


def show_stats(con, label):
    # MON$ tables are a stable snapshot per transaction: commit after each peek.
    cur = con.cursor()
    cur.execute('select r.MON$RECORD_UPDATES, r.MON$RECORD_IMGC, '
                '       r.MON$RECORD_PURGES, r.MON$RECORD_EXPUNGES, '
                '       r.MON$BACKVERSION_READS '
                'from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)')
    upd, imgc, purges, expunges, back = cur.fetchone()
    cur.close()
    con.commit()
    print(f'{label:<34} upd={upd:<4} imgc={imgc:<3} purges={purges:<3} '
          f'expunges={expunges:<3} backreads={back}')


def show_counters(con, label):
    i = con.info
    print(f'{label:<34} OIT={i.oit} OAT={i.oat} OST={i.ost} '
          f'Next={i.next_transaction} (sweep interval {i.sweep_interval})')


def main():
    path = db_path('gc_sweep')
    with recreate(path) as writer, attach(path) as pinner:
        execute(writer, 'create table gctest (id int primary key, val int)')
        execute(writer, 'insert into gctest values (1, 0)')

        # 1. Pin a snapshot: while it lives, version 0 must survive.
        snap = pinner.transaction_manager(SNAPSHOT)
        snap.begin()
        print('pinned SNAPSHOT reads val =', scalar(pinner, VAL, tr=snap))
        show_stats(writer, 'before updates:')

        # 2. Twelve committed updates -> twelve back versions... in theory.
        for i in range(1, 13):
            execute(writer, 'update gctest set val = ? where id = 1', (i,))
        show_stats(writer, 'after 12 updates (snapshot open):')
        print('pinned SNAPSHOT still reads val =', scalar(pinner, VAL, tr=snap))

        # 3. Release the snapshot; a scan now trips over the below-OST chain.
        snap.commit()
        snap.close()
        print('snapshot released; new reader sees val =', scalar(writer, VAL))
        writer.commit()
        time.sleep(1.5)
        show_stats(writer, 'after release + scan + 1.5s:')

        # 4. A committed DELETE older than the OST is expunged, not purged.
        execute(writer, 'delete from gctest where id = 1')
        scalar(writer, 'select count(*) from gctest')        # scan -> collect
        writer.commit()
        time.sleep(1.5)
        show_stats(writer, 'after DELETE + scan + 1.5s:')

        # 5. A no_auto_undo rollback is recorded only in the TIP: a stump
        #    that freezes the OIT until a sweep rewrites its state.
        show_counters(writer, 'header counters before rollback:')
        stump = writer.transaction_manager(STUMP)
        stump.begin()
        execute(writer, 'insert into gctest values (2, 0)', tr=stump)
        stump.rollback()
        stump.close()
        show_counters(writer, 'after no_auto_undo rollback:')

        # 6. The sweep, through the Services API (gfix -sweep's engine).
        with server() as srv:
            srv.database.sweep(database=path)
        show_counters(writer, 'after srv.database.sweep():')


if __name__ == '__main__':
    run(main)
