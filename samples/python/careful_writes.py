#
# careful_writes.py - kill a database engine mid-write; the file needs no
# recovery (Python twin of ../cpp/careful_writes.cpp; see
# ../../careful-writes-and-crash-safety.md).
#
# The writer attaches by plain local path, so the Y-valve loads the EMBEDDED
# engine into the writer process: SIGKILLing that process while it flushes
# pages of an uncommitted 500,000-row insert is a genuine engine crash, not
# a dropped client connection.  The parent then re-attaches and counts:
# committed marker present, uncommitted rows gone, attach instantaneous - no
# log replay, because there is no log.
#
# firebird-driver is a libfbclient binding, so the embedded engine is just a
# connection string away (unlike node-firebird's wire-only stack).  Where
# C++ fork()s, the parent re-runs this script with --writer through
# subprocess.Popen and kills it with Popen.kill() (SIGKILL on POSIX).
#
# Run:  python3 careful_writes.py [/tmp/fbhandson/careful_writes_py.fdb]
#
import os
import subprocess
import sys
import time

os.environ.setdefault('FIREBIRD', '/opt/firebird')   # embedded engine's root

from firebird.driver import connect, create_database  # noqa: E402

from fbsample import PASSWORD, USER, run              # noqa: E402

DEFAULT_DB = '/tmp/fbhandson/careful_writes_py.fdb'


def writer(path):
    """Child: be the engine.  Commit a marker, then write and never commit."""
    with create_database(path, user=USER, password=PASSWORD) as con:
        cur = con.cursor()
        cur.execute('create table cw (id int, tag varchar(30))')
        con.commit()
        cur.execute("insert into cw values (1, 'committed-marker')")
        con.commit()
        print(f'[writer {os.getpid()}] marker row committed (forced writes on)',
              flush=True)
        cur.execute("execute block as declare i int = 0; begin"
                    "  while (i < 500000) do begin"
                    "    insert into cw values (:i + 1000, 'uncommitted'); i = i + 1;"
                    "  end "
                    "end")                        # the crash victim, uncommitted
        print('[writer] bulk insert finished uncommitted; waiting for SIGKILL',
              flush=True)
        time.sleep(3600)


def size(path):
    try:
        return os.stat(path).st_size
    except FileNotFoundError:
        return -1


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DB
    if os.path.exists(path):
        os.unlink(path)                           # fresh run

    # 1. Spawn the writer: a separate process running the embedded engine.
    child = subprocess.Popen([sys.executable, __file__, '--writer', path])

    # 2. Wait until the file visibly grows - the engine is flushing freshly
    #    allocated pages of the uncommitted transaction - then kill -9 it.
    base = -1
    for _ in range(600):
        time.sleep(0.05)
        sz = size(path)
        if base < 0 and sz > 0:
            base = sz
        if base > 0 and sz > base + 2 * 1024 * 1024:
            print(f'file grew {base} -> {sz} bytes; SIGKILL to engine pid {child.pid}')
            break
    child.kill()
    child.wait()

    # 3. Re-attach at once.  There is no recovery step to run.
    t0 = time.monotonic()
    with connect(path, user=USER, password=PASSWORD) as con:
        cur = con.cursor()
        cur.execute("select count(*) from cw where tag = 'committed-marker'")
        committed = cur.fetchone()[0]
        cur.execute("select count(*) from cw where tag = 'uncommitted'")
        uncommitted = cur.fetchone()[0]
        cur.close()
        con.commit()
    ms = (time.monotonic() - t0) * 1000
    print(f're-attach + both counts took {ms:.0f} ms')
    print(f'committed marker rows : {committed}   <- survived the crash')
    print(f'uncommitted rows      : {uncommitted}   <- rolled back by visibility, not replay')


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--writer':
        run(lambda: writer(sys.argv[2]))
    else:
        run(main)
