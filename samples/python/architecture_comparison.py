#
# architecture_comparison.py - one client library, two ways into the engine
# (Python twin of ../cpp/architecture_comparison.cpp; see
# ../../architecture-comparison.md).
#
# Attachment [1] uses inet://localhost/employee, so the Y-valve inside
# libfbclient routes to the Remote provider and SQL crosses a socket to the
# server.  Attachment [2] uses a bare local path, so the same Y-valve loads
# the Engine provider into THIS Python process - no server at all.  For each
# one the engine is asked where it runs: ENGINE_VERSION, NETWORK_PROTOCOL
# and MON$SERVER_PID compared with os.getpid().
#
# firebird-driver is a ctypes binding of libfbclient, not a wire-protocol
# reimplementation, so both halves are reachable: connect() just hands the
# connection string to the Y-valve, and the string alone picks the provider.
# The driver also asks without SQL: con.info.firebird_version is the
# isc_info_firebird_version item, one line per layer the request crossed -
# remotely the server's engine plus the client's Remote provider, embedded
# the engine alone.
#
# Run:  python3 architecture_comparison.py [remote-db] [embedded-path]
#
import os
import sys

from firebird.driver import DatabaseError, connect, create_database

from fbsample import PASSWORD, USER, run

INSPECT = """
select rdb$get_context('SYSTEM', 'ENGINE_VERSION'),
       rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'),
       a.mon$server_pid
from mon$attachments a
where a.mon$attachment_id = current_connection"""


def open_db(database, create):
    try:
        return connect(database, user=USER, password=PASSWORD)
    except DatabaseError:
        if not create:
            raise
        return create_database(database, user=USER, password=PASSWORD)


def inspect(label, database, create):
    with open_db(database, create) as con:
        cur = con.cursor()
        cur.execute(INSPECT)
        version, protocol, server_pid = cur.fetchone()
        cur.close()
        con.commit()
        me = os.getpid()
        print(label)
        print('    connection string :', database)
        print('    ENGINE_VERSION    :', version)
        print('    NETWORK_PROTOCOL  :', protocol if protocol is not None else '<null>')
        print(f'    MON$SERVER_PID    : {server_pid}   (this process is pid {me}'
              f'{" -- the engine runs IN this process" if server_pid == me else ""})')
        # isc_info_firebird_version: one line per layer the call crossed
        for i, layer in enumerate(con.info.firebird_version.split('\n')):
            print('    info version      :' if i == 0 else '                       ', layer)


def main():
    remote = sys.argv[1] if len(sys.argv) > 1 else 'inet://localhost/employee'
    embedded = (sys.argv[2] if len(sys.argv) > 2
                else '/tmp/fbhandson/arch_embedded_py.fdb')
    print('One libfbclient, two providers behind the Y-valve.\n')
    inspect('[1] Remote provider (client-server):', remote, False)
    print()
    inspect('[2] Engine provider (embedded, no server):', embedded, True)
    print('\ndone.')


if __name__ == '__main__':
    run(main)
