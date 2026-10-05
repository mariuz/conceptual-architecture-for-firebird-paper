#
# sorting.py - the TempCacheLimit threshold made visible (Python twin of
# ../cpp/sorting.cpp; see ../../sorting-and-temp-space.md).
#
# Fills a table with 200,000 rows (~82 MB of sort data, 400-byte key), then
# runs two ORDER BY queries that both get a SORT plan: the big one (~82 MB)
# exceeds TempCacheLimit (64 MB) and spills, the small one (20,000 rows,
# ~8 MB) stays in TempSpace's memory cache.  While each query runs a
# watcher thread, on its OWN attachment, polls MON$MEMORY_USAGE at database
# level (a fresh transaction per poll: MON$ snapshots are per-transaction)
# and looks for the unlinked fb_sort_* scratch files.
#
# firebird-driver releases the GIL inside its ctypes calls, so a plain
# threading.Thread keeps polling while the main thread is blocked in the
# fetch that performs the sort.  The plan is the prepared Statement's
# .detailed_plan, whose Sort node carries the record and key lengths.
#
# The scratch files are unlinked on creation, so only the server's
# /proc/<pid>/fd shows them, and that needs the server's uid or root.  The
# sample reads it directly when it can; with FB_SORT_SUDO=1 it uses
# `sudo -n` like the C++ twin; otherwise it falls back to watching the free
# space of the scratch filesystem (/tmp, the default TempDirectories) drop:
# an unlinked file still occupies its blocks.
#
# Run:  python3 sorting.py [database]
#
import glob
import os
import subprocess
import threading

from firebird.driver import TPB, Isolation, TraAccessMode

from fbsample import attach, attach_or_create, db_path, execute, run, scalar

MEM = ('select m.mon$memory_allocated from mon$database d'
       '  join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id')
POLL_TPB = TPB(isolation=Isolation.SNAPSHOT, access_mode=TraAccessMode.READ,
               lock_timeout=0).get_buffer()
SCRATCH_FS = os.environ.get('FIREBIRD_TMP', '/tmp')


def scratch_via_proc(pid):
    """(files, bytes) of open fb_sort_* files, or None if /proc is closed."""
    try:
        sizes = [os.stat(fd).st_size for fd in glob.glob(f'/proc/{pid}/fd/*')
                 if 'fb_sort' in os.readlink(fd)]
        if not os.listdir(f'/proc/{pid}/fd'):
            return None
        return len(sizes), sum(sizes)
    except PermissionError:
        pass
    if os.environ.get('FB_SORT_SUDO') == '1':
        out = subprocess.run(
            f"sudo -n find /proc/{pid}/fd -lname '*fb_sort*' -print0 2>/dev/null"
            " | xargs -0 -r sudo -n stat -L -c %s 2>/dev/null",
            shell=True, capture_output=True, text=True).stdout.split()
        return len(out), sum(int(s) for s in out)
    return None


def free_bytes():
    st = os.statvfs(SCRATCH_FS)
    return st.f_bavail * st.f_frsize


class Watcher(threading.Thread):
    def __init__(self, path, pid):
        super().__init__()
        self.path, self.pid = path, pid
        self.stop = threading.Event()
        self.peak_mem = self.peak_files = self.peak_scratch = 0
        self.proc_visible = True
        self.free0 = free_bytes()
        self.peak_drop = 0

    def run(self):
        with attach(self.path) as mon:
            while not self.stop.is_set():
                seen = scratch_via_proc(self.pid)
                if seen is None:
                    self.proc_visible = False
                else:
                    self.peak_files = max(self.peak_files, seen[0])
                    self.peak_scratch = max(self.peak_scratch, seen[1])
                self.peak_drop = max(self.peak_drop, self.free0 - free_bytes())
                tm = mon.transaction_manager(POLL_TPB)   # new tx => new snapshot
                tm.begin()
                self.peak_mem = max(self.peak_mem, scalar(mon, MEM, tr=tm))
                tm.commit()
                tm.close()
                self.stop.wait(0.02)


def main():
    path = db_path('sorting')
    with attach_or_create(path) as db:
        execute(db, 'recreate table bulk (id integer, pad varchar(400) character set ascii)')
        execute(db, "execute block as declare i integer = 0; begin"
                    "  while (i < 200000) do begin"
                    "    insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x'));"
                    "    i = i + 1;"
                    "  end "
                    "end")
        print('bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data')

        pid = scalar(db, 'select mon$server_pid from mon$attachments'
                         ' where mon$attachment_id = current_connection')
        mem_idle = scalar(db, MEM)
        db.commit()
        print(f'server pid {pid}, database memory allocated while idle: {mem_idle} bytes')

        cases = (('big sort (200k rows, ~82 MB)',
                  'select first 1 id from bulk order by pad desc'),
                 ('small sort (20k rows, ~8 MB)',
                  'select first 1 id from bulk where mod(id, 10) = 0 order by pad desc'))
        for label, sql in cases:
            w = Watcher(path, pid)
            w.start()
            cur = db.cursor()
            stmt = cur.prepare(sql)
            sort = [ln.strip() for ln in stmt.detailed_plan.splitlines() if 'Sort' in ln]
            print(f'\n{label}\n  {stmt.plan.strip()}\n  {sort[0]}')
            cur.execute(stmt)                       # the sort happens here
            top = cur.fetchone()[0]
            cur.close()
            stmt.free()
            db.commit()
            w.stop.set()
            w.join()
            print(f'  top row id = {top}')
            if w.proc_visible:
                print(f'  peak fb_sort_* scratch: {w.peak_files} file(s), '
                      f'{w.peak_scratch} bytes')
            else:
                print(f'  /proc/{pid}/fd not readable; peak drop of free space on '
                      f'{SCRATCH_FS}: {w.peak_drop} bytes')
            print(f'  peak database MON$MEMORY_ALLOCATED: {w.peak_mem} bytes '
                  f'(+{w.peak_mem - mem_idle} over idle)')
    print('\ndone.')


if __name__ == '__main__':
    run(main)
