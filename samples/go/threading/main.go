// threading - SuperServer's thread-per-attachment topology watched from
// outside (Go twin of ../../cpp/threading.cpp; see
// ../../../threading-and-synchronization.md).
//
// MON$SERVER_PID names the engine process and /proc/<pid>/task counts its
// threads (same host), measured before, during and after twelve
// goroutines each hold their own attachment for two seconds.
// MON$ATTACHMENTS then shows the background workers (Cache Writer, Garbage
// Collector) as system attachments, and every attachment the same pid.
//
// Go's twist: the other twins open twelve connections by hand, one per
// thread.  Here twelve goroutines SHARE one *sql.DB - database/sql's pool
// is goroutine-safe - and each takes a dedicated *sql.Conn from it; the
// pool, allowed 12 open connections, dials one attachment per concurrent
// demand.  So "one attachment per worker" is the pool's doing, and closing
// the pool is what detaches them all.  (The other samples pin their pools
// to one connection so that each *sql.DB is exactly one attachment.)
//
// The census queries run in explicit transactions: MON$ snapshots are
// per-transaction, and the driver's autocommit is COMMIT RETAINING, which
// keeps the first snapshot alive.
//
// Run:  go run ./threading [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"sync"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func countThreads(pid int64) int {
	entries, err := os.ReadDir(fmt.Sprintf("/proc/%d/task", pid))
	if err != nil {
		return -1
	}
	return len(entries)
}

func main() {
	path := fbsample.DBPath("threading")
	// Create, then re-attach: the driver's create DPB carries no
	// isc_dpb_process_name, its attach DPB does (MON$REMOTE_PROCESS below).
	created, err := fbsample.Create(path)
	fbsample.Check(err)
	created.Close()
	db, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer db.Close()
	_, err = db.Exec("recreate table t (id int primary key, v int)")
	fbsample.Check(err)
	_, err = db.Exec("update or insert into t values (1, 0) matching (id)")
	fbsample.Check(err)

	var pid int64
	fbsample.Check(db.QueryRow("select mon$server_pid from mon$attachments" +
		" where mon$attachment_id = current_connection").Scan(&pid))
	fmt.Printf("engine process: pid %d, %d threads (1 attachment open)\n", pid, countThreads(pid))

	// One shared pool, twelve goroutines, one dedicated connection each.
	pool, err := sql.Open("firebirdsql", fbsample.DSN(path, "charset=UTF8"))
	fbsample.Check(err)
	pool.SetMaxOpenConns(12)
	var opened, done sync.WaitGroup
	release := make(chan struct{})
	for i := 0; i < 12; i++ {
		opened.Add(1)
		done.Add(1)
		go func() {
			defer done.Done()
			ctx := context.Background()
			conn, err := pool.Conn(ctx) // a new attachment: none is idle
			fbsample.Check(err)
			defer conn.Close()
			var n int
			fbsample.Check(conn.QueryRowContext(ctx, "select count(*) from t").Scan(&n))
			opened.Done()
			<-release // hold the attachment open
		}()
	}
	opened.Wait() // all twelve attached and queried
	// A fresh transaction for a fresh MON$ snapshot: the autocommit mode's
	// COMMIT RETAINING would keep showing the snapshot taken above.
	tx, err := db.Begin()
	fbsample.Check(err)
	var users, pids int
	fbsample.Check(tx.QueryRow("select count(*) from mon$attachments where mon$system_flag = 0").Scan(&users))
	fbsample.Check(tx.QueryRow("select count(distinct mon$server_pid) from mon$attachments").Scan(&pids))
	fbsample.Check(tx.Commit())
	fmt.Printf("with 12 extra attachments: %d threads | %d user attachments, %d distinct server pid\n",
		countThreads(pid), users, pids)

	time.Sleep(2 * time.Second)
	close(release)
	done.Wait()
	pool.Close() // returning a Conn only parks it in the pool; Close detaches
	time.Sleep(time.Second)
	fmt.Printf("after they detach:        %d threads (pooled, not destroyed)\n", countThreads(pid))

	// The engine's own workers hold real attachments, visible from SQL.
	tx, err = db.Begin()
	fbsample.Check(err)
	defer tx.Commit()
	rows, err := fbsample.Rows(tx, "select mon$attachment_id, mon$system_flag, trim(mon$user),"+
		" coalesce(mon$remote_process, '<internal>') from mon$attachments order by mon$attachment_id")
	fbsample.Check(err)
	fmt.Printf("  %2s %3s  %-18s %s\n", "ID", "SYS", "USER", "REMOTE_PROCESS")
	for _, r := range rows {
		fmt.Printf("  %2s %3s  %-18s %s\n", fbsample.Text(r[0]), fbsample.Text(r[1]),
			fbsample.Text(r[2]), fbsample.Text(r[3]))
	}
}
