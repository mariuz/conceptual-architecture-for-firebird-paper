#
# fbsample.py - shared boilerplate for the Python hands-on twins.
#
# Every sample demonstrates one companion document of the paper.  They use
# firebird-driver (https://github.com/FirebirdSQL/python3-driver), the
# FirebirdSQL project's own Python driver: like the C++ samples it loads
# libfbclient and drives the OO API (firebird/Interface.h) through ctypes,
# so the TPB / DPB / SPB bytes, the Services API, events and plans are all
# reachable from Python.
#
# Like the other twins, everything runs against the local server with
# scratch databases under /tmp/fbhandson (SYSDBA/masterkey, overridable via
# ISC_USER / ISC_PASSWORD; FB_HOST picks another server, FB_CLIENT_LIBRARY
# another libfbclient).
#
import os
import sys

from firebird.driver import (DatabaseError, connect, connect_server,
                             create_database, driver_config)

HOST = os.environ.get('FB_HOST', 'localhost')
USER = os.environ.get('ISC_USER', 'SYSDBA')
PASSWORD = os.environ.get('ISC_PASSWORD', 'masterkey')
SCRATCH = '/tmp/fbhandson'

_lib = os.environ.get('FB_CLIENT_LIBRARY')
if _lib is None and os.path.exists('/opt/firebird/lib/libfbclient.so'):
    _lib = '/opt/firebird/lib/libfbclient.so'
if _lib:
    driver_config.fb_client_library.value = _lib


def db_path(topic):
    """Server path of a topic's scratch database (argv[1] overrides it)."""
    if len(sys.argv) > 1:
        return sys.argv[1]
    return f'{SCRATCH}/{topic}_py.fdb'


def dsn(path):
    """inet:// connection string for a server-side path or alias."""
    return f'inet://{HOST}/{path}'


def attach(path, **kw):
    """Attach to an existing database over the remote protocol."""
    kw.setdefault('charset', 'UTF8')
    return connect(dsn(path), user=USER, password=PASSWORD, **kw)


def attach_or_create(path, **kw):
    """Attach, creating the database if the first attach fails."""
    try:
        return attach(path, **kw)
    except DatabaseError:
        kw.setdefault('charset', 'UTF8')
        return create_database(dsn(path), user=USER, password=PASSWORD, **kw)


def recreate(path, **kw):
    """A fresh scratch database: drop it if it exists, then create it."""
    try:
        attach(path).drop_database()
    except DatabaseError:
        pass
    kw.setdefault('charset', 'UTF8')
    return create_database(dsn(path), user=USER, password=PASSWORD, **kw)


def employee(**kw):
    """The demo server's employee database (alias `employee`)."""
    return attach(os.environ.get('FB_DATABASE', 'employee'), **kw)


def server():
    """A Services API connection to the server's own service_mgr.

    connect_server() takes the NAME of a registered server config, not a
    host: an unknown name falls back to a config with no host, which
    attaches the in-process (embedded) engine's service_mgr instead of the
    server's.  Register HOST with its host set, so the attach goes to
    <HOST>:service_mgr.
    """
    cfg = driver_config.get_server(HOST) or driver_config.register_server(HOST)
    cfg.host.value = HOST
    return connect_server(HOST, user=USER, password=PASSWORD)


def query(con, sql, params=None, tr=None):
    """Run a SELECT and return every row (a list of tuples)."""
    cur = (tr or con).cursor()
    cur.execute(sql, params or ())
    rows = cur.fetchall()
    cur.close()
    return rows


def scalar(con, sql, params=None, tr=None):
    """The first column of the first row."""
    rows = query(con, sql, params, tr)
    return rows[0][0] if rows else None


def execute(con, sql, params=None, tr=None, commit=True):
    """Run a statement on the connection's main transaction (or `tr`)."""
    cur = (tr or con).cursor()
    cur.execute(sql, params or ())
    cur.close()
    if commit and tr is None:
        con.commit()


def error_text(exc):
    """A DatabaseError's message, one status-vector entry per line."""
    return '\n'.join(line for line in str(exc).splitlines() if line.strip())


def run(main):
    """Run a sample's main(), exiting 1 with the engine's message on error."""
    try:
        main()
    except DatabaseError as e:
        print('error:', error_text(e), file=sys.stderr)
        sys.exit(1)
