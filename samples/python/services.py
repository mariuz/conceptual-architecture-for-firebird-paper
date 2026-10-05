#
# services.py - the Services API from client code: a service_mgr attach,
# information requests, an SPB-driven backup and the 1 KB ring-buffer
# polling loop (Python twin of ../cpp/services.cpp; see ../../services-api.md).
#
# The backup runs BURP_main - the real gbak - on a server thread; its
# verbose output arrives through the 1 KB svc_stdout ring buffer, drained
# here one isc_info_svc_line query at a time.  Every path is a SERVER path:
# the .fbk lands on the server's filesystem, owned by the server's user.
#
# firebird-driver maps the Services API onto typed objects: connect_server()
# builds the SPB_ATTACH, Server.info answers the information items
# (isc_info_svc_server_version, _implementation, _get_env, ...) as
# properties, and Server.database.backup(database=, backup=, verbose=True)
# assembles the isc_action_svc_backup SPB.  The drain is left visible on
# purpose: readline_timed() is exactly one isc_info_svc_line query (passing
# callback= to backup() would hide the same loop inside the driver).
# Note: connect_server() takes a registered server-config NAME, not a host;
# an unknown name silently attaches the in-process (embedded) service_mgr.
# fbsample.server() registers the host first, so this is a real
# localhost:service_mgr - the .fbk owner (the server's user) proves it.
#
# Run:  python3 services.py [database [backup-file]]
#
import sys

from firebird.driver import TIMEOUT, DatabaseError, SrvInfoCode

from fbsample import SCRATCH, attach_or_create, execute, run, server


def main():
    db_file = sys.argv[1] if len(sys.argv) > 1 else f'{SCRATCH}/services_py.fdb'
    bk_file = sys.argv[2] if len(sys.argv) > 2 else f'{SCRATCH}/services_py.fbk'

    # 0. Make sure the scratch database exists (idempotent).
    with attach_or_create(db_file) as db:
        try:
            execute(db, 'create table t (id int, v varchar(20))')
        except DatabaseError:
            db.rollback()                        # already there

    # 1. Attach to service_mgr; 2. information requests, no action needed.
    with server() as srv:
        print('service       :', srv.host)
        print('server version:', srv.info.get_info(SrvInfoCode.SERVER_VERSION),
              f'(info.version = {srv.info.version})')
        print('architecture  :', srv.info.architecture)
        print('home directory:', srv.info.home_directory)
        print('security db   :', srv.info.security_database)

        # 3. Start action_backup - dispatched to BURP_main on a server thread.
        srv.database.backup(database=db_file, backup=bk_file, verbose=True)
        print('backup started (verbose) - draining the 1 KB ring buffer:')

        # 4. The polling loop: one isc_info_svc_line query per call; the
        #    producer blocks whenever the buffer is full.
        lines = polls = 0
        last = None
        while True:
            line = srv.readline_timed(1)
            polls += 1
            if line is None:                     # the empty line: gbak finished
                break
            if line is TIMEOUT:
                continue
            lines += 1
            last = line.rstrip()
            if lines <= 3:
                print(' ', last)
            elif lines == 4:
                print('  ...')
        print(' ', last)
        print(f'done: {lines} gbak lines drained in {polls} query() polls')
        print(f'the file {bk_file} now exists on the SERVER, owned by the server\'s user')


if __name__ == '__main__':
    run(main)
