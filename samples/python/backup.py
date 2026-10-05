#
# backup.py - a gbak backup + restore round trip through the Services API
# (Python twin of ../cpp/backup.cpp; see ../../backup-and-recovery.md).
#
# Creates a scratch database with three rows, then - while that attachment
# is still open (gbak reads through a snapshot: the "online" property) -
# backs it up and restores it with REPLACE through service_mgr, streaming
# gbak's verbose log, and finally attaches to the restored copy to prove the
# rows survived.  No gbak binary is needed on the client.
#
# firebird-driver wraps the Services API natively: connect_server() is the
# service_mgr attachment, srv.database.backup()/restore() build the
# isc_action_svc_backup/_restore SPB from keyword arguments (verbose=True is
# isc_spb_verbose, SrvRestoreFlag.REPLACE is isc_spb_res_replace), and
# callback= receives gbak's log one line at a time - the driver runs the
# isc_info_svc_line drain loop the C++ sample writes by hand.  One trap:
# connect_server('localhost') does NOT mean the server on localhost (see
# remote_service_manager() below).
#
# Run:  python3 backup.py [database]
#
from firebird.driver import SrvRestoreFlag, connect_server, driver_config

from fbsample import HOST, PASSWORD, USER, attach, db_path, execute, recreate, run, scalar

FBK = '/tmp/fbhandson/backup_py.fbk'                 # server-side paths
RESTORED = '/tmp/fbhandson/backup_py_restored.fdb'


def remote_service_manager():
    """service_mgr on HOST, over the network.

    connect_server()'s first argument names a *registered server config*,
    not a host: an unknown name falls back to the defaults, whose host is
    unset, and the driver then attaches to plain 'service_mgr' - the
    EMBEDDED engine's service manager in this process.  Registering the
    host gives 'localhost:service_mgr', the server's own.
    """
    cfg = driver_config.get_server(HOST) or driver_config.register_server(HOST)
    cfg.host.value = HOST
    return connect_server(HOST, user=USER, password=PASSWORD)


def gbak(line):
    print('  gbak>', line.rstrip())     # each line arrives with its newline


def main():
    source = db_path('backup')
    with recreate(source) as con:                    # stays open: online backup
        execute(con, 'CREATE TABLE BR_ITEMS (ID INT NOT NULL PRIMARY KEY,'
                     ' NAME VARCHAR(30))')
        cur = con.cursor()
        cur.executemany('INSERT INTO BR_ITEMS VALUES (?, ?)',
                        [(1, 'alpha'), (2, 'beta'), (3, 'gamma')])
        cur.close()
        con.commit()
        print('source ready: BR_ITEMS with 3 rows')

        with remote_service_manager() as srv:
            print(f'\n== backup: {source} -> {FBK} ==')
            srv.database.backup(database=source, backup=FBK, verbose=True,
                                callback=gbak)

            print(f'\n== restore: {FBK} -> {RESTORED} ==')
            srv.database.restore(backup=FBK, database=RESTORED,
                                 flags=SrvRestoreFlag.REPLACE, verbose=True,
                                 callback=gbak)

    with attach(RESTORED) as rdb:
        print(f'\nrestored database says: {scalar(rdb, "SELECT COUNT(*) FROM BR_ITEMS")}'
              f' rows, max name = {scalar(rdb, "SELECT MAX(NAME) FROM BR_ITEMS")}')
        rdb.commit()
    print('done.')


if __name__ == '__main__':
    run(main)
