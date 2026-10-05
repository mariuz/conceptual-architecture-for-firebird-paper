// request_lifecycle - one CREATE TABLE round trip, instrumented from the
// client (Go twin of ../../cpp/request_lifecycle.cpp; see
// ../../../request-lifecycle-code-trace.md).
//
// Each step is timed and the worker attachment's MON$IO_STATS /
// MON$RECORD_STATS counters are sampled around it:
//
//	prepare  -> tx.Prepare: op_prepare_statement (Stages 1-5)
//	execute  -> stmt.Exec: EXE/MET catalog STOREs - record inserts jump
//	commit   -> tx.Commit: TRA_commit -> DFW -> CCH_flush -> page writes jump
//
// database/sql shapes the instrumentation the way Rust's borrow checker
// did: the worker *sql.DB is pinned to one connection, and an open *sql.Tx
// owns it, so nothing else can run on that attachment until the Tx ends.
// The MON$ samples therefore come from a second, monitor attachment that
// reads the worker's counters by attachment id, in a fresh transaction per
// sample (MON$ snapshots are frozen per transaction) - which also gives the
// outside view of the uncommitted catalog row.  One gap: firebirdsql reads
// the statement type at prepare time but keeps it private, so the "DDL"
// verdict the C++ twin prints is not available to a database/sql program.
//
// Run:  go run ./request_lifecycle [database]
package main

import (
	"database/sql"
	"fmt"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

type stats struct{ fetches, marks, writes, recIns int64 }

// sample reads the worker's cumulative counters in a fresh transaction.
func sample(mon *sql.DB, attID int64) stats {
	tx, err := mon.Begin()
	fbsample.Check(err)
	defer tx.Commit()
	var s stats
	fbsample.Check(tx.QueryRow(
		"SELECT i.MON$PAGE_FETCHES, i.MON$PAGE_MARKS, i.MON$PAGE_WRITES,"+
			"       r.MON$RECORD_INSERTS"+
			" FROM MON$ATTACHMENTS a"+
			" JOIN MON$IO_STATS i ON a.MON$STAT_ID = i.MON$STAT_ID"+
			" JOIN MON$RECORD_STATS r ON a.MON$STAT_ID = r.MON$STAT_ID"+
			" WHERE a.MON$ATTACHMENT_ID = ?", attID).Scan(&s.fetches, &s.marks, &s.writes, &s.recIns))
	return s
}

const countDemo = "SELECT COUNT(*) FROM RDB$RELATIONS WHERE RDB$RELATION_NAME = 'TRACE_DEMO'"

func outside(mon *sql.DB) int64 {
	var n int64
	fbsample.Check(mon.QueryRow(countDemo).Scan(&n))
	return n
}

func ms(t0 time.Time) float64 { return float64(time.Since(t0).Microseconds()) / 1000 }

func main() {
	path := fbsample.DBPath("request_lifecycle")
	db, err := fbsample.Create(path)
	fbsample.Check(err)
	defer db.Close()
	mon, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer mon.Close()

	db.Exec("DROP TABLE trace_demo") // idempotency: ignore "does not exist"
	var attID int64
	fbsample.Check(db.QueryRow("SELECT CURRENT_CONNECTION FROM RDB$DATABASE").Scan(&attID))
	s0 := sample(mon, attID)

	tx, err := db.Begin()
	fbsample.Check(err)

	// -- prepare: remote -> DSQL (Stages 1-5) -------------------------------
	t0 := time.Now()
	stmt, err := tx.Prepare("CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY," +
		" name VARCHAR(30))")
	fbsample.Check(err)
	fmt.Printf("prepare  %6.2f ms   (statement type: kept private by the driver)\n", ms(t0))

	// -- execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) ---------
	t0 = time.Now()
	_, err = stmt.Exec()
	fbsample.Check(err)
	tExec := ms(t0)
	s1 := sample(mon, attID)
	fmt.Printf("execute  %6.2f ms   catalog record inserts: +%d, page marks: +%d\n",
		tExec, s1.recIns-s0.recIns, s1.marks-s0.marks)
	var inside int64
	fbsample.Check(tx.QueryRow(countDemo).Scan(&inside))
	fmt.Printf("         in this tx:  RDB$RELATIONS has TRACE_DEMO = %d\n", inside)
	fmt.Printf("         monitor:     RDB$RELATIONS has TRACE_DEMO = %d  (TRA_commit has not happened)\n",
		outside(mon))
	fbsample.Check(stmt.Close())

	// -- commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9) ------
	t0 = time.Now()
	fbsample.Check(tx.Commit())
	tCommit := ms(t0)
	s2 := sample(mon, attID)
	fmt.Printf("commit   %6.2f ms   page writes: +%d  (fetches: +%d over the whole trip)\n",
		tCommit, s2.writes-s1.writes, s2.fetches-s0.fetches)
	fmt.Printf("         monitor:     RDB$RELATIONS has TRACE_DEMO = %d\n", outside(mon))
	fmt.Println("done.")
}
