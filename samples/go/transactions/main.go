// transactions - MVCC isolation seen from the client (Go twin of
// ../../cpp/transactions_demo.cpp; see ../../../transactions-and-concurrency.md).
//
// Two attachments play the same scenario: SNAPSHOT stability, READ
// COMMITTED freshness, then a write conflict.  firebirdsql maps
// database/sql's isolation levels onto fixed TPBs: LevelRepeatableRead is
// isc_tpb_concurrency (SNAPSHOT), LevelReadCommitted is read_committed +
// rec_version, LevelSerializable is consistency - all of them WAIT.  The
// only NO WAIT level it offers is its own LevelReadCommittedNoWait, so a
// SNAPSHOT conflict cannot fail fast here: like node-firebird, the losing
// update BLOCKS until the holder commits and only then raises the
// conflict, and the sample commits the blocker after 300 ms to let it
// surface.
//
// Run:  go run ./transactions [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const selectAmount = "select amount from balance where id = 1"

func begin(db *sql.DB, level sql.IsolationLevel) *sql.Tx {
	tx, err := db.BeginTx(context.Background(), &sql.TxOptions{Isolation: level})
	fbsample.Check(err)
	return tx
}

func amount(tx *sql.Tx) int {
	var n int
	fbsample.Check(tx.QueryRow(selectAmount).Scan(&n))
	return n
}

func main() {
	path := fbsample.DBPath("tx")
	a, err := fbsample.Create(path) // attachment A
	fbsample.Check(err)
	defer a.Close()
	b, err := fbsample.Attach(path) // attachment B
	fbsample.Check(err)
	defer b.Close()

	_, err = a.Exec("recreate table balance (id integer primary key, amount integer)")
	fbsample.Check(err)
	_, err = a.Exec("insert into balance values (1, 100)")
	fbsample.Check(err)

	// --- 1. SNAPSHOT stability ------------------------------------------
	snapA := begin(a, sql.LevelRepeatableRead)
	fmt.Println("A (SNAPSHOT)       sees amount =", amount(snapA))

	_, err = b.Exec("update balance set amount = 999 where id = 1")
	fbsample.Check(err)
	fmt.Println("B                  committed amount = 999")

	fmt.Println("A (same SNAPSHOT)  sees amount =", amount(snapA),
		"  <- still the start-of-tx version")
	fbsample.Check(snapA.Commit())

	// --- 2. READ COMMITTED sees the new version ---------------------------
	// database/sql gives one attachment one transaction at a time (A's pool
	// holds one connection), so the SNAPSHOT above is closed first.
	rcA := begin(a, sql.LevelReadCommitted)
	fmt.Println("A (READ COMMITTED) sees amount =", amount(rcA),
		"  <- the committed version")
	fbsample.Check(rcA.Commit())

	// --- 3. Write conflict (WAIT) ------------------------------------------
	holdB := begin(b, sql.LevelRepeatableRead)
	_, err = holdB.Exec("update balance set amount = amount + 1 where id = 1")
	fbsample.Check(err)

	loserA := begin(a, sql.LevelRepeatableRead)
	race := make(chan error, 1)
	go func() {
		_, err := loserA.Exec("update balance set amount = amount + 10 where id = 1")
		race <- err
	}()
	time.Sleep(300 * time.Millisecond) // let A block on the row
	fbsample.Check(holdB.Commit())     // blocker commits -> conflict fires
	if err := <-race; err != nil {
		fmt.Println("A conflicting update failed as designed:")
		fmt.Println("    " + strings.ReplaceAll(fbsample.ErrText(err), "\n", "\n    "))
		if fe, ok := fbsample.FbError(err); ok {
			fmt.Printf("    sqlcode %d / sqlstate %s / gds %v\n", fe.SQLCode, fe.SQLState, fe.GDSCodes)
		}
	} else {
		fmt.Println("unexpected: conflicting update succeeded")
	}
	loserA.Rollback()
	fmt.Println("done.")
}
