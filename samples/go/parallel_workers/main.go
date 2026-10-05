// parallel_workers - the knobs, the poll, an honest zero, and the
// parallelism that is not the engine's (Go twin of
// ../../cpp/parallel_workers.cpp; see ../../../parallel-workers.md).
//
// [A] ParallelWorkers and MaxParallelWorkers are GLOBAL config, readable
// by any client through RDB$CONFIG, with the attachment's own grant in
// MON$ATTACHMENTS.MON$PARALLEL_WORKERS.  firebirdsql builds its DPB from a
// fixed list with no isc_dpb_parallel_workers, so the SQL route cannot
// even ask.
//
// [B] Build a table spanning several pointer pages, CREATE INDEX on it and
// poll MON$ATTACHMENTS from a second attachment (a goroutine) for
// '<Worker>' rows.  A pure-wire driver has no embedded engine, so the C++
// twin's private-firebird.conf phase is the door closed here, as for
// node-firebird: against the live server (MaxParallelWorkers = 1) the
// honest result is zero.
//
// [C] The driver's Services BackupManager CAN ask for parallelism -
// WithBackupParallelWorkers(4) puts isc_spb_bkp_parallel_workers in the
// SPB - and the server honours it, because gbak's parallel backup is not
// the engine's worker pool: the in-server gbak opens its own reader
// attachments sharing one snapshot (burp/backup.epp, BurpTasks.cpp),
// unbounded by MaxParallelWorkers.  The same poller sees them appear.
//
// Two driver details shape the code.  Every poll is a real transaction:
// db.QueryRow alone auto-commits with COMMIT RETAINING, which keeps the
// MON$ snapshot frozen (see ../monitoring).  And the long statements run in
// explicit transactions: firebirdsql reads the auto-commit COMMIT
// RETAINING reply with a fixed 10 s deadline, so an auto-committed bulk
// insert or CREATE INDEX (whose build runs at commit) fails with
// "i/o timeout"; an explicit Commit() waits as long as it takes.
//
// Run:  go run ./parallel_workers [database]    (~1 min: 200k-row table)
package main

import (
	"database/sql"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/nakagami/firebirdsql"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// inTx runs one long statement in an explicit transaction (auto-commit's
// COMMIT RETAINING reply is read with a fixed 10 s deadline).
func inTx(db *sql.DB, query string) {
	tx, err := db.Begin()
	fbsample.Check(err)
	_, err = tx.Exec(query)
	fbsample.Check(err)
	fbsample.Check(tx.Commit())
}

func knobs(db *sql.DB) string {
	rows, err := fbsample.Rows(db, `select trim(rdb$config_name), rdb$config_value from rdb$config
	                                 where rdb$config_name in ('ParallelWorkers', 'MaxParallelWorkers')
	                                 order by rdb$config_name desc`)
	fbsample.Check(err)
	parts := []string{}
	for _, r := range rows {
		parts = append(parts, fbsample.Text(r[0])+" = "+fbsample.Text(r[1]))
	}
	return strings.Join(parts, ", ")
}

var widest [][]any

// count runs a count(*) in a fresh MON$ snapshot: a real transaction per
// poll, because auto-commit's COMMIT RETAINING would keep the snapshot.
func count(mon *sql.DB, query string) int {
	tx, err := mon.Begin()
	fbsample.Check(err)
	defer tx.Commit()
	var n int
	fbsample.Check(tx.QueryRow(query).Scan(&n))
	return n
}

// poll runs work while a goroutine polls query on mon every 20 ms; it
// returns the widest count seen and the number of polls.
func poll(mon *sql.DB, query string, work func()) (maxSeen, polls int) {
	stop := make(chan struct{})
	result := make(chan [2]int)
	go func() {
		m, n := 0, 0
		for {
			select {
			case <-stop:
				result <- [2]int{m, n}
				return
			default:
			}
			if c := count(mon, query); c > m {
				m = c
				tx, _ := mon.Begin()
				widest, _ = fbsample.Rows(tx, `select mon$attachment_id, trim(mon$user), mon$system_flag
				                                 from mon$attachments order by 1`)
				tx.Commit()
			}
			n++
			time.Sleep(20 * time.Millisecond)
		}
	}()
	work()
	close(stop)
	r := <-result
	return r[0], r[1]
}

func main() {
	path := fbsample.DBPath("parallel")
	db, err := fbsample.Create(path)
	fbsample.Check(err)
	defer db.Close()

	// -- [A] the knobs and the grant ------------------------------------
	fmt.Println("[A] server attach (firebirdsql's DPB has no isc_dpb_parallel_workers)")
	granted, err := fbsample.Scalar(db, `select mon$parallel_workers from mon$attachments
	                                      where mon$attachment_id = current_connection`)
	fbsample.Check(err)
	fmt.Printf("    server config: %s; granted MON$PARALLEL_WORKERS = %v\n", knobs(db), granted)

	// -- [B] something parallelizable, and the poll -----------------------
	fmt.Println("\n[B] CREATE INDEX on the live server, MON$ATTACHMENTS polled meanwhile")
	_, err = db.Exec("recreate table parade (id int, val varchar(200))")
	fbsample.Check(err)
	// Incompressible filler: getMaxWorkers() goes parallel only if the
	// relation spans more than one pointer page.
	inTx(db, `execute block as declare n int = 0; begin
	                    while (n < 200000) do begin
	                      insert into parade values (:n,
	                        uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) ||
	                        uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) ||
	                        uuid_to_char(gen_uuid()));
	                      n = n + 1;
	                    end end`)
	ptr, err := fbsample.Scalar(db, `select count(*) from rdb$pages p join rdb$relations r
	                                   on p.rdb$relation_id = r.rdb$relation_id
	                                where r.rdb$relation_name = 'PARADE' and p.rdb$page_type = 4`)
	fbsample.Check(err)
	fmt.Printf("    parade table: 200000 rows of 180 incompressible bytes, %v pointer pages\n", ptr)

	mon, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer mon.Close()
	var elapsed time.Duration
	seen, polls := poll(mon, "select count(*) from mon$attachments where mon$user = '<Worker>'", func() {
		t0 := time.Now()
		inTx(db, "create index ix_parade on parade (val)") // built at commit (DFW)
		elapsed = time.Since(t0)
	})
	fmt.Printf("    create index: %d ms; '<Worker>' attachments seen in %d polls: %d\n",
		elapsed.Milliseconds(), polls, seen)
	if seen == 0 {
		fmt.Println("    zero, as configured: MaxParallelWorkers = 1 caps the engine's worker pool")
	}

	// -- [C] a parallel request the server does honour: gbak's own readers --
	fmt.Println("\n[C] BackupManager.Backup(..., WithBackupParallelWorkers(4)) of the same database")
	bm, err := firebirdsql.NewBackupManager(fbsample.ServiceAddr(), fbsample.User,
		fbsample.Password, fbsample.ServiceOptions())
	fbsample.Check(err)
	fbk := strings.TrimSuffix(path, ".fdb") + ".fbk"
	var said string
	// User attachments other than this sample's two: the in-server gbak's.
	// User attachments other than this sample's own two are the in-server gbak's.
	var mine, monID int64
	fbsample.Check(db.QueryRow("select current_connection from rdb$database").Scan(&mine))
	fbsample.Check(mon.QueryRow("select current_connection from rdb$database").Scan(&monID))
	seen, polls = poll(mon, fmt.Sprintf(`select count(*) from mon$attachments
	    where mon$system_flag = 0 and mon$attachment_id not in (%d, %d)`, mine, monID), func() {
		verbose := make(chan string)
		done := make(chan error, 1)
		go func() {
			done <- bm.Backup(path, fbk,
				firebirdsql.NewBackupOptions(firebirdsql.WithBackupParallelWorkers(4)), verbose)
			close(verbose)
		}()
		for line := range verbose {
			if strings.Contains(line, "workers") {
				said = strings.TrimSpace(line)
			}
		}
		fbsample.Check(<-done)
	})
	os.Remove(fbk)
	fmt.Println("    verbose: " + said)
	fmt.Printf("    gbak's own attachments seen in %d polls: up to %d\n", polls, seen)
	fmt.Println("    MON$ATTACHMENTS at the widest moment:")
	for _, r := range widest {
		who := ""
		switch r[0].(int64) {
		case mine:
			who = "   <- this sample"
		case monID:
			who = "   <- the poller"
		}
		fmt.Printf("        %-3v %s  (system_flag %v)%s\n", r[0], fbsample.Text(r[1]), r[2], who)
	}
	fmt.Println("done.")
}
