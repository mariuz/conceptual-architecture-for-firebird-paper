// monitoring - the MON$ hierarchy and its stable snapshot (Go twin of
// ../../cpp/monitoring.cpp; see ../../../monitoring-and-tuning.md).
//
// Walks MON$DATABASE -> MON$ATTACHMENTS -> MON$TRANSACTIONS ->
// MON$STATEMENTS down to this very connection, reads its own counters
// through the MON$STAT_ID join, runs a 10 000-row full-scan workload
// INSIDE the same transaction and shows the counters frozen (the first
// MON$ select took a stable snapshot), then refreshed in a new transaction.
//
// database/sql's trap is node-firebird's: outside a *sql.Tx every
// statement auto-commits, so the freeze needs an explicit
// db.BeginTx(LevelRepeatableRead).  But firebirdsql auto-commits with
// COMMIT RETAINING on one long-lived transaction, and the sample shows
// what that does to MON$: COMMIT RETAINING keeps the transaction - and its
// MON$ snapshot - alive, so plain db.QueryRow calls stay frozen across the
// same workload too, until a real transaction boundary (a db.Begin()
// ... Commit()) refreshes them.  The pure-wire driver has no attachment
// info calls, so the Python twin's second, live channel
// (isc_info_fetches, per-table read counts) is out of reach here.
//
// Run:  go run ./monitoring [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const counters = `
    SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS,
           R.MON$RECORD_INSERTS, I.MON$PAGE_FETCHES
      FROM MON$ATTACHMENTS A
      JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID
      JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID
     WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION`

type querier interface {
	QueryRow(string, ...any) *sql.Row
}

func showCounters(q querier, label string) {
	var seq, idx, ins, fetches int64
	fbsample.Check(q.QueryRow(counters).Scan(&seq, &idx, &ins, &fetches))
	fmt.Printf("%-38s seq_reads=%-6d idx_reads=%-5d inserts=%-6d page_fetches=%d\n",
		label, seq, idx, ins, fetches)
}

func workload(q querier) {
	var count, point int
	fbsample.Check(q.QueryRow("SELECT COUNT(*) FROM MON_WORK").Scan(&count))
	fbsample.Check(q.QueryRow("SELECT VAL FROM MON_WORK WHERE ID = 4242").Scan(&point))
	fmt.Printf("count = %d, point = %d\n", count, point)
}

func main() {
	db, err := fbsample.Create(fbsample.DBPath("monitoring"))
	fbsample.Check(err)
	defer db.Close()

	// Workload table: 10000 rows to scan.
	_, err = db.Exec("RECREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)")
	fbsample.Check(err)
	_, err = db.Exec(`EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN
	                    WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I);
	                    I = I + 1; END
	                  END`)
	fbsample.Check(err)

	// -- 1. the hierarchy, one level per query, one consistent snapshot --
	tx, err := db.BeginTx(context.Background(), &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
	fbsample.Check(err)
	fmt.Println("== MON$DATABASE: transaction markers ==")
	var oit, oat, next, bufs int64
	fbsample.Check(tx.QueryRow(`SELECT MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE,
	                                   MON$NEXT_TRANSACTION, MON$PAGE_BUFFERS FROM MON$DATABASE`).
		Scan(&oit, &oat, &next, &bufs))
	fmt.Printf("OIT=%d OAT=%d NEXT=%d page_buffers=%d\n", oit, oat, next, bufs)

	fmt.Println("\n== MON$ATTACHMENTS -> MON$TRANSACTIONS -> MON$STATEMENTS (me) ==")
	rows, err := fbsample.Rows(tx, `
	    SELECT A.MON$ATTACHMENT_ID, A.MON$USER, T.MON$TRANSACTION_ID, S.MON$STATE,
	           CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 40) AS VARCHAR(40))
	      FROM MON$ATTACHMENTS A
	      JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID
	      JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID
	     WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION`)
	fbsample.Check(err)
	for _, r := range rows {
		fmt.Printf("attachment %v (%s), tx %v, state %v: %s\n", r[0],
			strings.TrimSpace(fbsample.Text(r[1])), r[2], r[3],
			strings.Join(strings.Fields(fbsample.Text(r[4])), " "))
	}

	// -- 2. the snapshot property, measured on our own counters ---------
	fmt.Println()
	showCounters(tx, "MON$ snapshot 1:")
	fmt.Println("\n... running workload: SELECT COUNT(*) full scan + indexed lookup ...")
	workload(tx)
	fmt.Println()
	showCounters(tx, "same transaction: STILL snapshot 1:")
	fbsample.Check(tx.Commit())

	// The auto-commit transaction's first MON$ read takes a fresh snapshot.
	fmt.Println()
	showCounters(db, "auto-commit, first MON$ read: fresh")

	// -- 3. the auto-commit path: COMMIT RETAINING after every statement --
	fmt.Println("\n... same workload outside a Tx (auto-commit = COMMIT RETAINING) ...")
	workload(db)
	showCounters(db, "auto-commit, after workload: STILL")
	fresh, err := db.Begin()
	fbsample.Check(err)
	showCounters(fresh, "explicit new transaction: fresh")
	fbsample.Check(fresh.Commit())
	fmt.Println("\ndone.")
}
