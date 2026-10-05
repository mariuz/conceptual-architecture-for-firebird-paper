// memory_pools - the pool hierarchy made visible from SQL via
// MON$MEMORY_USAGE (Go twin of ../../cpp/memory_pools.cpp; see
// ../../../memory-management.md).
//
// The six-group summary with the parent-redirection signature (child pools
// with real MON$MEMORY_USED and zero MON$MEMORY_ALLOCATED), one
// connection's database -> attachment -> transaction chain, and a
// transaction pool growing under an uncommitted 3000-row UPDATE, then
// vanishing at rollback - watched from a second attachment, because a MON$
// snapshot is frozen per transaction.  database/sql makes the freshness
// rule explicit: every observation is its own mon.Begin() ... Commit(), and
// the worker's growing transaction is a *sql.Tx whose ids come from SQL
// (current_connection / current_transaction) - the pure-Go driver has no
// attachment/transaction info calls to read them from, unlike the Python
// twin.  The SUM over the BIGINT columns arrives as INT128, which
// firebirdsql decodes natively (to its decimal string, via big.Int), so -
// unlike the rsfbclient twin - no CAST AS BIGINT is needed.
//
// Run:  go run ./memory_pools [database]
package main

import (
	"database/sql"
	"fmt"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// snapshot runs one query in its own transaction: a fresh MON$ snapshot.
func snapshot(mon *sql.DB, query string, args ...any) [][]any {
	tx, err := mon.Begin()
	fbsample.Check(err)
	defer tx.Commit()
	rows, err := fbsample.Rows(tx, query, args...)
	fbsample.Check(err)
	return rows
}

func levelSummary(mon *sql.DB) {
	rows := snapshot(mon, `select MON$STAT_GROUP, count(*), sum(MON$MEMORY_USED),
	                              sum(MON$MEMORY_ALLOCATED), count(nullif(MON$MEMORY_ALLOCATED, 0))
	                         from MON$MEMORY_USAGE group by 1 order by 1`)
	fmt.Println("GROUP POOLS USED       ALLOCATED  WITH_OWN_EXTENTS")
	for _, r := range rows {
		fmt.Printf("%-5v %-5v %-10v %-10v %v\n", r[0], r[1], r[2], r[3], r[4])
	}
}

// poolRow prints one row of the worker's pool chain and returns its used bytes.
func poolRow(mon *sql.DB, label, join string, args ...any) int64 {
	rows := snapshot(mon, "select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE "+join, args...)
	if len(rows) == 0 {
		return -1
	}
	fmt.Printf("  %-24s used=%-10v allocated=%v\n", label, rows[0][0], rows[0][1])
	return rows[0][0].(int64)
}

const (
	byAtt = "join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ?"
	byTra = "join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = ?"
)

func main() {
	path := fbsample.DBPath("memory_pools")
	worker, err := fbsample.Create(path)
	fbsample.Check(err)
	defer worker.Close()
	mon, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer mon.Close()

	_, err = worker.Exec("recreate table t (id int, pad varchar(200))")
	fbsample.Check(err)
	_, err = worker.Exec(`execute block as declare i int = 0; begin
	                        while (i < 3000) do begin
	                          insert into t values (:i, rpad('x', 200, 'x')); i = i + 1;
	                        end
	                      end`)
	fbsample.Check(err)

	fmt.Println("-- per-level summary (0=db 1=att 2=tra 3=stmt 5=cmp; used > 0 with allocated = 0: parent redirection)")
	levelSummary(mon)

	// The worker's own chain: database -> attachment -> transaction.
	tx, err := worker.Begin()
	fbsample.Check(err)
	var att, tra int64
	fbsample.Check(tx.QueryRow("select current_connection, current_transaction from rdb$database").Scan(&att, &tra))

	fmt.Printf("\n-- worker's pool chain (attachment %d, transaction %d; before the update)\n", att, tra)
	poolRow(mon, "database pool:", "join MON$DATABASE using (MON$STAT_ID)")
	poolRow(mon, "worker attachment pool:", byAtt, att)
	poolRow(mon, "worker transaction pool:", byTra, tra)

	// Grow the transaction pool: the undo log of an uncommitted UPDATE
	// lives in the transaction's pool.
	_, err = tx.Exec("update t set pad = rpad('y', 200, 'y')")
	fbsample.Check(err)

	fmt.Println("\n-- after an uncommitted 3000-row UPDATE in that transaction")
	attBefore := poolRow(mon, "worker attachment pool:", byAtt, att)
	traPool := poolRow(mon, "worker transaction pool:", byTra, tra)

	fbsample.Check(tx.Rollback()) // bulk-free: the whole pool goes at once
	fmt.Println("\n-- after rollback (transaction pool destroyed with its undo log)")
	attAfter := poolRow(mon, "worker attachment pool:", byAtt, att)
	fmt.Printf("  attachment used fell by %d; the dead transaction pool held %d\n",
		attBefore-attAfter, traPool)
}
