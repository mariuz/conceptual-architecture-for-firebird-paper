// metadata_cache - the metadata cache's visibility rule from two attachments
// (Go twin of ../../cpp/metadata_cache.cpp; see ../../../metadata-cache.md).
//
//  1. an uncommitted ALTER is visible to its own transaction and to nobody
//     else - B gets "Column unknown" while A already selects the new column;
//  2. a committed version is visible to everyone at once, even to a
//     statement prepared inside B's older, still-open SNAPSHOT transaction:
//     metadata is read-committed, not snapshot-isolated;
//  3. two concurrent uncommitted DDLs on one object collide in newVersion;
//  4. every ALTER appended a row to RDB$FORMATS.
//
// database/sql makes the transaction boundaries - the whole experiment -
// explicit by construction: each *sql.DB here is pinned to one attachment,
// so while a *sql.Tx is open that attachment has nothing else to run
// statements on, and no query can silently ride a hidden default
// transaction.  B's SNAPSHOT is sql.LevelRepeatableRead (isc_tpb_concurrency).
// Errors come back as *firebirdsql.FbError: the formatted chain plus the
// GDS codes behind it.
//
// Run:  go run ./metadata_cache [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func begin(db *sql.DB, level sql.IsolationLevel) *sql.Tx {
	tx, err := db.BeginTx(context.Background(), &sql.TxOptions{Isolation: level})
	fbsample.Check(err)
	return tx
}

func exec(tx *sql.Tx, query string) {
	_, err := tx.Exec(query)
	fbsample.Check(err)
}

// tryQuery prints either the first value or the engine's error, on one line.
func tryQuery(who string, tx *sql.Tx, query string) {
	v, err := fbsample.Scalar(tx, query)
	if err != nil {
		fmt.Printf("%s: %s -> ERROR: %s\n", who, query,
			strings.ReplaceAll(fbsample.ErrText(err), "\n", " "))
		return
	}
	fmt.Printf("%s: %s -> %s\n", who, query, fbsample.Text(v))
}

func main() {
	path := fbsample.DBPath("mdc")
	A, err := fbsample.Recreate(path)
	fbsample.Check(err)
	defer A.Close()
	B, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer B.Close()

	t := begin(A, sql.LevelReadCommitted)
	exec(t, "recreate table t (a integer)")
	fbsample.Check(t.Commit())
	t = begin(A, sql.LevelReadCommitted)
	exec(t, "insert into t values (1)")
	fbsample.Check(t.Commit())

	// -- 1. uncommitted DDL: mine, and mine alone ---------------------------
	fmt.Println("== 1. uncommitted ALTER: visible to creator only ==")
	aDdl := begin(A, sql.LevelRepeatableRead) // A's DDL stays uncommitted
	exec(aDdl, "alter table t add e integer")
	tryQuery("A (same tx)  ", aDdl, "select e from t")
	bTra := begin(B, sql.LevelRepeatableRead)
	tryQuery("B            ", bTra, "select e from t")
	fbsample.Check(bTra.Commit())

	// -- 2. committed DDL ignores open snapshots ----------------------------
	fmt.Println("\n== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==")
	bTra = begin(B, sql.LevelRepeatableRead) // explicit SNAPSHOT
	tryQuery("B (snapshot) ", bTra, "select count(*) from t")
	fbsample.Check(aDdl.Commit()) // E becomes committed
	t = begin(A, sql.LevelRepeatableRead)
	exec(t, "alter table t add d integer")
	fbsample.Check(t.Commit()) // D committed after B's snapshot
	tryQuery("B (same  tx) ", bTra, "select d from t")
	fmt.Println("   (records are snapshot-isolated; metadata is read-committed —\n" +
		"    the new statement was prepared against the chain's current head)")
	fbsample.Check(bTra.Commit())

	// -- 3. concurrent DDL: the newVersion collision ------------------------
	fmt.Println("\n== 3. two uncommitted DDLs on one object ==")
	aDdl = begin(A, sql.LevelRepeatableRead)
	exec(aDdl, "alter table t add f integer")
	bTra = begin(B, sql.LevelRepeatableRead)
	if _, err := bTra.Exec("alter table t add g integer"); err != nil {
		fmt.Printf("B: ALTER failed:\n%s\n", fbsample.ErrText(err))
		if fe, ok := fbsample.FbError(err); ok {
			fmt.Printf("   (sqlcode %d, gds %v)\n", fe.SQLCode, fe.GDSCodes)
		}
	} else {
		fmt.Println("B: ALTER unexpectedly succeeded")
	}
	bTra.Rollback()
	aDdl.Rollback() // F vanishes with the rollback

	// -- 4. the on-disk half: one format per committed shape ----------------
	fmt.Println("\n== 4. RDB$FORMATS after the committed DDL ==")
	t = begin(A, sql.LevelReadCommitted)
	n, err := fbsample.Scalar(t, "select count(*) from rdb$formats f "+
		"join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id "+
		"where r.rdb$relation_name = 'T'")
	fbsample.Check(err)
	fmt.Printf("formats stored for T: %v (T has lived through that many shapes)\n", n)
	rows, err := fbsample.Rows(t, "select a, e, d from t")
	fbsample.Check(err)
	fmt.Println("A E      D")
	for _, r := range rows {
		fmt.Printf("%s %-6s %s\n", fbsample.Text(r[0]), fbsample.Text(r[1]), fbsample.Text(r[2]))
	}
	fbsample.Check(t.Commit())
	fmt.Println("done.")
}
