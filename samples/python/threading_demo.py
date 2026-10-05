#
# threading_demo.py - SuperServer's thread-per-attachment topology watched
# from outside (Python twin of ../cpp/threading.cpp; see
# ../../threading-and-synchronization.md).  Not named threading.py: that
# would shadow the standard library module it uses.
#
# MON$SERVER_PID names the engine process and /proc/<pid>/task counts its
# threads (same host), measured before, during and after twelve client
# threads each open their own attachment and hold it for two seconds.
# MON$ATTACHMENTS then shows the background workers (Cache Writer, Garbage
# Collector) as system attachments, and every attachment the same pid.
#
# firebird-driver declares DB-API threadsafety = 1 - threads may share the
# module but not connections - so each worker opens its own; and since the
# driver's ctypes calls release the GIL, the twelve attaches really do
# overlap on the wire.  The GIL limits Python bytecode, not the server.
#
# Run:  python3 threading_demo.py [database]
#
import os
import threading
import time

import firebird.driver

from fbsample import attach, attach_or_create, db_path, execute, query, run, scalar


def count_threads(pid):
    return len(os.listdir(f'/proc/{pid}/task'))


def worker(path, opened):
    with attach(path) as w:
        scalar(w, 'select count(*) from t')
        w.commit()
        opened.release()
        time.sleep(2)


def main():
    path = db_path('threading')
    with attach_or_create(path) as db:
        execute(db, 'recreate table t (id int primary key, v int)')
        execute(db, 'update or insert into t values (1, 0) matching (id)')
        pid = scalar(db, 'select mon$server_pid from mon$attachments'
                         ' where mon$attachment_id = current_connection')
        db.commit()
        print(f'driver threadsafety = {firebird.driver.threadsafety}')
        print(f'engine process: pid {pid}, {count_threads(pid)} threads (1 attachment open)')

        opened = threading.Semaphore(0)
        workers = [threading.Thread(target=worker, args=(path, opened)) for _ in range(12)]
        for w in workers:
            w.start()
        for _ in workers:
            opened.acquire()                 # all twelve attached and queried
        users = scalar(db, 'select count(*) from mon$attachments where mon$system_flag = 0')
        pids = scalar(db, 'select count(distinct mon$server_pid) from mon$attachments')
        db.commit()
        print(f'with 12 extra attachments: {count_threads(pid)} threads | {users} user '
              f'attachments, {pids} distinct server pid')

        for w in workers:
            w.join()
        time.sleep(1)
        print(f'after they detach:        {count_threads(pid)} threads (pooled, not destroyed)')

        print(f'{"ID":>4} {"SYS":>3}  {"USER":<18} REMOTE_PROCESS')
        for att_id, sys_flag, user, proc in query(
                db, "select mon$attachment_id, mon$system_flag, trim(mon$user),"
                    "       coalesce(mon$remote_process, '<internal>')"
                    "  from mon$attachments order by mon$attachment_id"):
            print(f'{att_id:>4} {sys_flag:>3}  {user:<18} {proc}')
        db.commit()


if __name__ == '__main__':
    run(main)
