#
# embedded_demo.py - the full server engine, loaded into the Python process
# (Python twin of ../cpp/embedded_demo.cpp; see
# ../../embedded-architecture-comparison.md).
#
# Three demonstrations:
#   1. /proc/self/maps before and after: firebird-driver loads libfbclient
#      lazily (ctypes, on first API use), and the first local-path attach
#      makes its Y-valve load the Engine provider (libEngine14.so) - three
#      states, like fb-cpp's and fbintf's runtime loading;
#   2. real work with no server: DDL, DML and a query against a local .fdb
#      created by this process (NETWORK_PROTOCOL is NULL, the engine's pid
#      is ours);
#   3. the continuum measured: the same connect() timed against a local path
#      and against inet://localhost/employee.
#
# firebird-driver is a libfbclient binding, so - unlike node-firebird - the
# embedded half is just a connection string without inet://.
#
# Run:  python3 embedded_demo.py [local-path] [remote-db]
#
import os
import sys
import time

os.environ.setdefault('FIREBIRD', '/opt/firebird')   # embedded engine's root

from firebird.driver import (DatabaseError, connect,  # noqa: E402
                             create_database)
from firebird.driver.fbapi import get_api              # noqa: E402

from fbsample import PASSWORD, USER, run               # noqa: E402


def mapped(fragment):
    with open('/proc/self/maps') as maps:
        return 'yes' if fragment in maps.read() else 'no'


def state(label):
    print(f'{label:<22} libfbclient mapped={mapped("libfbclient")},'
          f' libEngine14 mapped={mapped("libEngine14")}')


def attach_ms(database):
    t0 = time.perf_counter()
    connect(database, user=USER, password=PASSWORD).close()
    return (time.perf_counter() - t0) * 1000


def main():
    local = sys.argv[1] if len(sys.argv) > 1 else '/tmp/fbhandson/embedded_demo_py.fdb'
    remote = sys.argv[2] if len(sys.argv) > 2 else 'inet://localhost/employee'

    # --- 1. watch the client, then the engine, arrive ---------------------
    state('before any API use:')
    get_api()
    state('after get_api():')
    try:
        con = connect(local, user=USER, password=PASSWORD)
    except DatabaseError:
        con = create_database(local, user=USER, password=PASSWORD)
    state('after local attach:')
    print()

    # --- 2. real work with no server anywhere ----------------------------
    with con:
        cur = con.cursor()
        cur.execute('recreate table gadgets (id int primary key, name varchar(20))')
        con.commit()
        cur.executemany('insert into gadgets values (?, ?)',
                        [(1, 'sprocket'), (2, 'flange'), (3, 'grommet')])
        con.commit()
        cur.execute("select count(*), max(name),"
                    " coalesce(rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'),"
                    "          '<null: in-process>'), a.mon$server_pid"
                    " from gadgets, mon$attachments a"
                    " where a.mon$attachment_id = current_connection"
                    " group by 3, 4")
        rows, max_name, protocol, engine_pid = cur.fetchone()
        cur.close()
        con.commit()
        print(f'rows={rows}  max(name)={max_name}  NETWORK_PROTOCOL={protocol}')
        print(f"engine pid={engine_pid}, my pid={os.getpid()} — the 'server' is this process\n")

        # --- 3. attach cost: in-process call vs socket + SRP handshake ----
        #     (`con` stays open, as in the C++ twin: the engine keeps the
        #     database open, so each timed attach is not a cold file open)
        runs = 5
        attach_ms(local)                               # warm-ups
        attach_ms(remote)
        emb = sum(attach_ms(local) for _ in range(runs)) / runs
        rem = sum(attach_ms(remote) for _ in range(runs)) / runs
        print(f'attach+detach avg over {runs} runs:')
        print(f'    embedded  {local:<38} {emb:7.2f} ms')
        print(f'    remote    {remote:<38} {rem:7.2f} ms')
    print('done.')


if __name__ == '__main__':
    run(main)
