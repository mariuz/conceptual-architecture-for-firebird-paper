#
# page_cache.py - one hot-page ping-pong, two cache topologies (Python twin
# of ../cpp/page_cache.cpp; see ../../page-cache-coherency.md).
#
#   phase 1 - two client processes -> ONE SuperServer shared cache
#             (coherency by shared memory; almost no physical I/O)
#   phase 2 - two EMBEDDED engine processes with PRIVATE caches over one
#             file (ServerMode=SuperClassic sandbox; coherency by LCK_bdb
#             page locks + blocking ASTs - data travels through the disk)
#
# MON$IO_STATS per attachment is the referee.  The parent only spawns
# children (sys.executable re-running this file with --init / --worker /
# --check); each phase-2 child IS a full engine.  firebird-driver loads
# libfbclient, so the embedded door is open just as for the C++ twin: a
# DSN without inet:// goes to the Engine provider, and the FIREBIRD
# environment variable handed to the child points it at the sandbox.
#
# Run:  python3 page_cache.py
#
import os
import subprocess
import sys

from firebird.driver import DatabaseError, connect, create_database

from fbsample import PASSWORD, SCRATCH, USER, dsn, run

SRV_DB = dsn(f'{SCRATCH}/page_cache_srv_py.fdb')
EMB_DB = f'{SCRATCH}/page_cache_emb_py.fdb'          # no host: embedded engine
SANDBOX = f'{SCRATCH}/fbemb_py'
ROUNDS = 300


def open_db(conn, create=False):
    kw = dict(user=USER, password=PASSWORD, charset='UTF8')
    if create:
        try:
            connect(conn, **kw).drop_database()
        except DatabaseError:
            pass
        return create_database(conn, **kw)
    return connect(conn, **kw)


def init_db(conn):                        # --init: fresh table, two rows
    with open_db(conn, create=True) as db:
        cur = db.cursor()
        cur.execute('create table t (id int primary key, v int)')
        db.commit()
        cur.execute('insert into t values (1, 0)')
        cur.execute('insert into t values (2, 0)')
        db.commit()


def worker(conn, row_id):                 # --worker: 300 commits on one row
    with open_db(conn) as db:
        cur = db.cursor()
        for _ in range(ROUNDS):
            cur.execute('update t set v = v + 1 where id = ?', (int(row_id),))
            db.commit()
        cur.execute('select MON$PAGE_FETCHES, MON$PAGE_READS, MON$PAGE_WRITES '
                    'from MON$IO_STATS join MON$ATTACHMENTS using (MON$STAT_ID) '
                    'where MON$ATTACHMENT_ID = CURRENT_CONNECTION')
        fetches, reads, writes = cur.fetchone()
        db.commit()
        print(f'  worker pid {os.getpid():<6} row {row_id}: {ROUNDS} commits | '
              f'page fetches={fetches:<6} reads={reads:<4} writes={writes}', flush=True)


def check(conn):                          # --check: any lost updates?
    with open_db(conn) as db:
        cur = db.cursor()
        cur.execute('select id, v from t order by id')
        for row_id, v in cur.fetchall():
            print(f'  final: id={row_id} v={v} (expected {ROUNDS})', flush=True)
        db.commit()


def spawn(*args, env=None):
    return subprocess.Popen([sys.executable, __file__, *args], env=env)


def phase(conn, env=None):
    spawn('--init', conn, env=env).wait()
    workers = [spawn('--worker', conn, '1', env=env),
               spawn('--worker', conn, '2', env=env)]
    for w in workers:
        w.wait()
    spawn('--check', conn, env=env).wait()


def build_sandbox():
    """A FIREBIRD root whose firebird.conf says SuperClassic: each embedded
    process locks the file SHARED and runs its own page cache."""
    os.makedirs(SANDBOX, exist_ok=True)
    for f in ('plugins', 'intl', 'tzdata', 'firebird.msg', 'security6.fdb'):
        link = os.path.join(SANDBOX, f)
        if not os.path.lexists(link):
            os.symlink(f'/opt/firebird/{f}', link)
    with open(os.path.join(SANDBOX, 'firebird.conf'), 'w') as conf:
        conf.write('ServerMode = SuperClassic\n')
    return dict(os.environ, FIREBIRD=SANDBOX)


def main():
    if len(sys.argv) > 2:
        role = sys.argv[1]
        if role == '--init':
            return init_db(sys.argv[2])
        if role == '--worker':
            return worker(sys.argv[2], sys.argv[3])
        if role == '--check':
            return check(sys.argv[2])

    print('phase 1: two client processes, ONE SuperServer shared cache', flush=True)
    phase(SRV_DB)
    print('phase 2: two EMBEDDED engine processes, PRIVATE page caches', flush=True)
    phase(EMB_DB, env=build_sandbox())
    print('same workload - the private caches paid for coherency in disk I/O.')


if __name__ == '__main__':
    run(main)
