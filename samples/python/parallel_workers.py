#
# parallel_workers.py - watching worker attachments appear, and be refused
# (Python twin of ../cpp/parallel_workers.cpp; see ../../parallel-workers.md).
#
# [A] Against the live server: ask for 4 workers via isc_dpb_parallel_workers.
#     Both knobs are GLOBAL config, so with the stock MaxParallelWorkers = 1
#     the engine clamps the request and attaches with a WARNING.
# [B] Against an embedded engine whose private FIREBIRD root sets
#     ParallelWorkers = 4 / MaxParallelWorkers = 8: build a 200k-row table of
#     incompressible rows, CREATE INDEX on it, and poll MON$ATTACHMENTS from a
#     second attachment (a Python thread) while the build runs.  The workers
#     appear as '<Worker>' attachments with MON$SYSTEM_FLAG = 1 and stay
#     pooled after the build.
#
# firebird-driver has the knob as a typed DPB field, but connect() takes it
# only through a registered database configuration (driver_config
# .register_database(...).parallel_workers) - not as a keyword.  The engine's
# reply is not lost as in fb-cpp/fbintf: status-vector WARNINGS surface as
# Python warnings (FirebirdWarning via the warnings module), so the sample
# catches isc_bad_par_workers with warnings.catch_warnings(record=True).
# Phase B is a dsn without inet:// - libfbclient's Engine provider runs the
# engine in this process, reading $FIREBIRD/firebird.conf.  The poller thread
# works because ctypes releases the GIL around every libfbclient call.
#
# Run:  python3 parallel_workers.py    (~30 s: builds a 40 MB scratch table)
#
import os

ROOT = '/tmp/fbhandson/fbroot-parallel-py'
os.environ['FIREBIRD'] = ROOT       # before libfbclient loads: read by phase B

import threading  # noqa: E402
import time  # noqa: E402
import warnings  # noqa: E402

from firebird.driver import (DatabaseError, connect, create_database,  # noqa: E402
                             driver_config)

from fbsample import (PASSWORD, USER, attach_or_create, dsn, query,  # noqa: E402
                      run, scalar)

SERVER_DB = '/tmp/fbhandson/parallel_py.fdb'
EMBEDDED_DB = '/tmp/fbhandson/par_embedded_py.fdb'


def make_root():
    """A private $FIREBIRD root: symlinks into the stock install, own firebird.conf."""
    os.makedirs(ROOT, exist_ok=True)
    for f in ('plugins', 'intl', 'firebird.msg', 'tzdata', 'plugins.conf', 'databases.conf'):
        link = os.path.join(ROOT, f)
        if not os.path.lexists(link):
            os.symlink('/opt/firebird/' + f, link)
    with open(os.path.join(ROOT, 'firebird.conf'), 'w') as conf:
        conf.write('ServerMode = Super\nParallelWorkers = 4\nMaxParallelWorkers = 8\n')


def knobs(con):
    return ', '.join(f'{name.strip()} = {value}' for name, value in query(con,
        "select rdb$config_name, rdb$config_value from rdb$config "
        "where rdb$config_name in ('ParallelWorkers', 'MaxParallelWorkers') "
        "order by rdb$config_name desc"))


def phase_a():
    attach_or_create(SERVER_DB).close()          # ensure it exists
    cfg = driver_config.register_database('parallel_server')
    cfg.database.value = dsn(SERVER_DB)
    cfg.parallel_workers.value = 4
    print('[A] server attach, isc_dpb_parallel_workers = 4')
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        con = connect('parallel_server', user=USER, password=PASSWORD)
    for w in caught:
        print(f'    {type(w.message).__name__}: {str(w.message).strip()}')
    with con:
        granted = scalar(con, 'select mon$parallel_workers from mon$attachments '
                              'where mon$attachment_id = current_connection')
        print(f'    server config: {knobs(con)}; granted MON$PARALLEL_WORKERS = '
              f'{granted} -> 0 extra workers\n')
        con.commit()


def phase_b():
    try:
        con = connect(EMBEDDED_DB, user=USER, charset='UTF8')
    except DatabaseError:
        con = create_database(EMBEDDED_DB, user=USER, charset='UTF8')
    with con:
        print(f'[B] embedded attach, FIREBIRD={ROOT}')
        print(f'    engine config: {knobs(con)}')
        cur = con.cursor()
        cur.execute('recreate table parade (id int, val varchar(200))')
        con.commit()
        # Incompressible filler: getMaxWorkers() goes parallel only if the
        # relation spans more than one pointer page.
        cur.execute('execute block as declare n int = 0; begin '
                    '  while (n < 200000) do begin '
                    '    insert into parade values (:n, '
                    '      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || '
                    '      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || '
                    '      uuid_to_char(gen_uuid())); '
                    '    n = n + 1; '
                    '  end end')
        con.commit()
        ptr_pages = scalar(con, "select count(*) from rdb$pages p join rdb$relations r "
                                "  on p.rdb$relation_id = r.rdb$relation_id "
                                "where r.rdb$relation_name = 'PARADE' and p.rdb$page_type = 4")
        print(f'    parade table: 200000 rows of 180 incompressible bytes, '
              f'{ptr_pages} pointer pages')

        seen = {'max': 0, 'roster': []}
        stop = threading.Event()

        def poller():
            with connect(EMBEDDED_DB, user=USER, charset='UTF8') as mon:
                while not stop.is_set():
                    n = scalar(mon, "select count(*) from mon$attachments "
                                    "where mon$user = '<Worker>'")
                    if n > seen['max']:
                        seen['max'] = n
                        seen['roster'] = query(mon,
                            'select trim(mon$user), mon$system_flag '
                            'from mon$attachments order by mon$attachment_id')
                    mon.commit()                 # fresh MON$ snapshot next time
                    time.sleep(0.02)

        thread = threading.Thread(target=poller)
        thread.start()
        t0 = time.monotonic()
        cur.execute('create index ix_parade on parade (val)')
        con.commit()
        ms = (time.monotonic() - t0) * 1000
        stop.set()
        thread.join()
        cur.close()

        print(f"    create index: {ms:.0f} ms; max '<Worker>' attachments seen: {seen['max']}")
        print('    MON$ATTACHMENTS at the widest moment:')
        for user, flag in seen['roster']:
            print(f'        {user}  (system_flag {flag})')
        print('    after build: workers stay pooled (idle timeout 60 s):',
              scalar(con, "select count(*) from mon$attachments where mon$user = '<Worker>'"))
        con.commit()


def main():
    make_root()
    phase_a()
    phase_b()
    print('done.')


if __name__ == '__main__':
    run(main)
