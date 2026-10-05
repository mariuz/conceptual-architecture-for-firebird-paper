#
# deployment.py - the deployment as the engine reports it to a client
# (Python twin of ../cpp/deployment.cpp; see ../../deployment-and-operations.md).
#
# The same three SQL layers as the C++ twin - MON$DATABASE (physical facts),
# RDB$CONFIG (the EFFECTIVE configuration: firebird.conf merged with
# databases.conf) and the SYSTEM context variables - plus the two views
# firebird-driver exposes WITHOUT any SQL:
#   - con.info: typed isc_info_* items of the attachment (page size, ODS,
#     buffers, sweep interval, write mode as an enum ...);
#   - srv.info: the service manager's isc_info_svc_* items - the install
#     tree an operator would look up in a shell (home, security database,
#     lock directory, attached databases).
# Read-only: safe against the shared employee database.
#
# Run:  python3 deployment.py [database]
#
import sys

from firebird.driver import connect_server, driver_config

from fbsample import HOST, PASSWORD, USER, attach, query, run, scalar


def remote_service_manager():
    """service_mgr of the server on HOST (connect_server()'s argument is a
    registered config name; an unknown one means the EMBEDDED manager)."""
    cfg = driver_config.get_server(HOST) or driver_config.register_server(HOST)
    cfg.host.value = HOST
    return connect_server(HOST, user=USER, password=PASSWORD)


def table(con, sql):
    cur = con.cursor()
    cur.execute(sql)
    names = [d[0] for d in cur.description]
    rows = [['<null>' if v is None else str(v) for v in r] for r in cur.fetchall()]
    cur.close()
    widths = [max([len(n)] + [len(r[i]) for r in rows]) for i, n in enumerate(names)]
    print(' '.join(n.ljust(w) for n, w in zip(names, widths)).rstrip())
    print(' '.join('-' * w for w in widths))
    for r in rows:
        print(' '.join(v.ljust(w) for v, w in zip(r, widths)).rstrip())


def main():
    database = sys.argv[1] if len(sys.argv) > 1 else 'employee'
    with attach(database) as con:
        def line(label, value):
            print(f'  {label:<22} {value}')

        print('== MON$DATABASE: the database as deployed ==')
        (name, ods_major, ods_minor, page_size, buffers, sweep, fw, dialect,
         crypt) = query(con, 'SELECT MON$DATABASE_NAME, MON$ODS_MAJOR,'
                             ' MON$ODS_MINOR, MON$PAGE_SIZE, MON$PAGE_BUFFERS,'
                             ' MON$SWEEP_INTERVAL, MON$FORCED_WRITES,'
                             ' MON$SQL_DIALECT, MON$CRYPT_STATE FROM MON$DATABASE')[0]
        for label, value in (('database file', name),
                             ('ODS version', f'{ods_major}.{ods_minor}'),
                             ('page size', page_size), ('page buffers', buffers),
                             ('sweep interval', sweep), ('forced writes', fw),
                             ('SQL dialect', dialect), ('crypt state', crypt)):
            line(label, value)

        n = scalar(con, 'SELECT COUNT(*) FROM RDB$CONFIG')
        print(f'\n== RDB$CONFIG: effective configuration (selected of {n} settings) ==')
        table(con, "SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_IS_SET"
                   " FROM RDB$CONFIG WHERE RDB$CONFIG_NAME IN ('ServerMode',"
                   " 'DefaultDbCachePages', 'DatabaseAccess', 'WireCrypt',"
                   " 'MaxParallelWorkers', 'SecurityDatabase')"
                   " ORDER BY RDB$CONFIG_NAME")

        print('\n== settings explicitly set in config files ==')
        table(con, 'SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_SOURCE'
                   ' FROM RDB$CONFIG WHERE RDB$CONFIG_IS_SET ORDER BY RDB$CONFIG_ID')

        print('\n== SYSTEM context: this engine, this session ==')
        for var in ('ENGINE_VERSION', 'DB_NAME', 'NETWORK_PROTOCOL',
                    'WIRE_CRYPT_PLUGIN', 'CLIENT_ADDRESS'):
            line(var, scalar(con, f"SELECT RDB$GET_CONTEXT('SYSTEM', '{var}')"
                                  " FROM RDB$DATABASE"))
        con.commit()

        print('\n== con.info: the same facts as isc_info_* items, no SQL ==')
        info = con.info
        line('name', info.name)
        line('ODS', info.ods)
        line('page size', info.page_size)
        line('page cache size', info.page_cache_size)
        line('sweep interval', info.sweep_interval)
        line('write mode', info.write_mode.name)
        line('pages allocated', info.pages_allocated)

        print('\n== srv.info: the install tree, from the service manager ==')
        with remote_service_manager() as srv:
            line('server version', srv.info.version)
            line('architecture', srv.info.architecture)
            line('home directory', srv.info.home_directory)
            line('security database', srv.info.security_database)
            line('lock directory', srv.info.lock_directory)
            line('message directory', srv.info.message_directory)
            line('attached databases', ', '.join(srv.info.attached_databases))
    print('\ndone.')


if __name__ == '__main__':
    run(main)
