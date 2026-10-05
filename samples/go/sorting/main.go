// sorting - the TempCacheLimit threshold made visible (Go twin of
// ../../cpp/sorting.cpp; see ../../../sorting-and-temp-space.md).
//
// Fills a table with 200,000 rows (~82 MB of sort data, 400-byte key), then
// runs two ORDER BY queries that both get a SORT plan: the big one (~82 MB)
// exceeds TempCacheLimit (64 MB) and spills, the small one (20,000 rows,
// ~8 MB) stays in TempSpace's memory cache.  While each query runs, a
// watcher goroutine on its OWN attachment (its own *sql.DB) polls
// MON$MEMORY_USAGE at database level - a fresh transaction per poll,
// because MON$ snapshots are per-transaction - and looks for the unlinked
// fb_sort_* scratch files.
//
// A goroutine needs no GIL story: the pure-Go driver blocks only the
// goroutine waiting on its socket.  firebirdsql has no plan API, so the
// plan comes from Firebird 6's RDB$SQL.EXPLAIN, whose Sort node carries
// the record and key lengths.  The scratch files are unlinked on
// creation, so only the server's /proc/<pid>/fd shows them, which needs
// the server's uid or root: the sample reads it when it can, uses
// `sudo -n` with FB_SORT_SUDO=1 like the C++ twin, and otherwise falls
// back to the free space of the scratch filesystem (/tmp, the default
// TempDirectories) dropping - an unlinked file still occupies its blocks.
//
// Run:  go run ./sorting [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const mem = "select m.mon$memory_allocated from mon$database d" +
	"  join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id"

func scratchFS() string {
	if v := os.Getenv("FIREBIRD_TMP"); v != "" {
		return v
	}
	return "/tmp"
}

func freeBytes() int64 {
	var st syscall.Statfs_t
	if syscall.Statfs(scratchFS(), &st) != nil {
		return 0
	}
	return int64(st.Bavail) * st.Bsize
}

// scratchViaProc returns (files, bytes) of open fb_sort_* files, ok=false
// when the server's fd table is closed to us.
func scratchViaProc(pid int64) (int64, int64, bool) {
	fds, err := filepath.Glob(fmt.Sprintf("/proc/%d/fd/*", pid))
	if err == nil && len(fds) > 0 {
		var files, total int64
		for _, fd := range fds {
			if target, err := os.Readlink(fd); err == nil && strings.Contains(target, "fb_sort") {
				if st, err := os.Stat(fd); err == nil {
					files++
					total += st.Size()
				}
			}
		}
		return files, total, true
	}
	if os.Getenv("FB_SORT_SUDO") == "1" {
		out, _ := exec.Command("sh", "-c", fmt.Sprintf(
			"sudo -n find /proc/%d/fd -lname '*fb_sort*' -print0 2>/dev/null"+
				" | xargs -0 -r sudo -n stat -L -c %%s 2>/dev/null", pid)).Output()
		var files, total int64
		for _, f := range strings.Fields(string(out)) {
			n, _ := strconv.ParseInt(f, 10, 64)
			files++
			total += n
		}
		return files, total, true
	}
	return 0, 0, false
}

type watcher struct {
	stop                          chan struct{}
	done                          sync.WaitGroup
	peakMem, peakFiles, peakBytes int64
	peakDrop                      int64
	procVisible                   bool
}

func watch(path string, pid int64) *watcher {
	w := &watcher{stop: make(chan struct{}), procVisible: true}
	mon, err := fbsample.Attach(path) // second attachment: MON$ polling
	fbsample.Check(err)
	free0 := freeBytes()
	w.done.Add(1)
	go func() {
		defer w.done.Done()
		defer mon.Close()
		for {
			select {
			case <-w.stop:
				return
			default:
			}
			if files, n, ok := scratchViaProc(pid); ok {
				w.peakFiles = max(w.peakFiles, files)
				w.peakBytes = max(w.peakBytes, n)
			} else {
				w.procVisible = false
			}
			w.peakDrop = max(w.peakDrop, free0-freeBytes())
			// New transaction => new MON$ snapshot.
			tx, err := mon.BeginTx(context.Background(), &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
			fbsample.Check(err)
			var m int64
			fbsample.Check(tx.QueryRow(mem).Scan(&m))
			tx.Commit()
			w.peakMem = max(w.peakMem, m)
			time.Sleep(20 * time.Millisecond)
		}
	}()
	return w
}

func main() {
	path := fbsample.DBPath("sorting")
	db, err := fbsample.Create(path)
	fbsample.Check(err)
	defer db.Close()

	_, err = db.Exec("recreate table bulk (id integer, pad varchar(400) character set ascii)")
	fbsample.Check(err)
	_, err = db.Exec("execute block as declare i integer = 0; begin" +
		"  while (i < 200000) do begin" +
		"    insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x'));" +
		"    i = i + 1;" +
		"  end " +
		"end")
	fbsample.Check(err)
	fmt.Println("bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data")

	var pid, memIdle int64
	fbsample.Check(db.QueryRow("select mon$server_pid from mon$attachments" +
		" where mon$attachment_id = current_connection").Scan(&pid))
	fbsample.Check(db.QueryRow(mem).Scan(&memIdle))
	fmt.Printf("server pid %d, database memory allocated while idle: %d bytes\n", pid, memIdle)

	cases := []struct{ label, sql string }{
		{"big sort (200k rows, ~82 MB)", "select first 1 id from bulk order by pad desc"},
		{"small sort (20k rows, ~8 MB)", "select first 1 id from bulk where mod(id, 10) = 0 order by pad desc"},
	}
	for _, c := range cases {
		// The plan, from SQL: no getPlan() in this driver.
		// (The Sort node shares an EXPLAIN row with the Refetch above it.)
		var access string
		fbsample.Check(db.QueryRow("select first 1 cast(access_path as varchar(300))"+
			"  from rdb$sql.explain(?) where access_path like '%Sort%' order by plan_line",
			c.sql).Scan(&access))
		sortNode := access
		for _, l := range strings.Split(access, "\n") {
			if strings.Contains(l, "Sort") {
				sortNode = strings.TrimSpace(l)
			}
		}

		w := watch(path, pid)
		var top int64
		fbsample.Check(db.QueryRow(c.sql).Scan(&top)) // the sort happens here
		close(w.stop)
		w.done.Wait()

		fmt.Printf("\n%s\n  %s\n", c.label, sortNode)
		fmt.Printf("  top row id = %d\n", top)
		if w.procVisible {
			fmt.Printf("  peak fb_sort_* scratch: %d file(s), %d bytes\n", w.peakFiles, w.peakBytes)
		} else {
			fmt.Printf("  /proc/%d/fd not readable; peak drop of free space on %s: %d bytes\n",
				pid, scratchFS(), w.peakDrop)
		}
		fmt.Printf("  peak database MON$MEMORY_ALLOCATED: %d bytes (+%d over idle)\n",
			w.peakMem, w.peakMem-memIdle)
	}
	fmt.Println("\ndone.")
}
