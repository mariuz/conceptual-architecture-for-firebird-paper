// lock_manager - the lock manager's three wait modes and its deadlock scan,
// timed from the client (Go twin of ../../cpp/lock_manager.cpp; see
// ../../../lock-manager.md).
//
// The C++ twin takes a genuine LCK_relation lock with SET TRANSACTION ...
// RESERVING t1 FOR PROTECTED WRITE.  firebirdsql cannot: the TPB is chosen
// for you from database/sql's isolation levels (no isc_tpb_lock_write, no
// isc_tpb_lock_timeout), and SET TRANSACTION is not a statement it can run.
// So, like the node-firebird and rsfbclient twins, it probes through ROW
// conflicts - waiting on a locked record is waiting on the blocker's
// LCK_tra lock - and each lck_wait mode is reached the Go way:
//
//	NO WAIT        -> the driver's LevelReadCommittedNoWait (isc_tpb_nowait)
//	"timeout 3 s"  -> a WAIT transaction plus context.WithTimeout(3 s): the
//	                  driver's cancel watcher sends op_cancel when the
//	                  deadline expires and the engine aborts the wait.  A
//	                  client-side deadline, not isc_tpb_lock_timeout.
//	WAIT           -> sql.LevelReadCommitted (isc_tpb_wait), granted when
//	                  the holder commits
//
// The second act crosses two SNAPSHOT WAIT updates from two goroutines and
// clocks the periodic deadlock scan (DeadlockTimeout, 10 s by default).
//
// Run:  go run ./lock_manager [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
	"github.com/nakagami/firebirdsql"
)

func firstLine(err error) string {
	return strings.SplitN(fbsample.ErrText(err), "\n", 2)[0]
}

func begin(db *sql.DB, level sql.IsolationLevel) *sql.Tx {
	tx, err := db.BeginTx(context.Background(), &sql.TxOptions{Isolation: level})
	fbsample.Check(err)
	return tx
}

// probe updates the contested row in a fresh transaction and reports how
// long the lock manager kept it waiting.
func probe(ctx context.Context, db *sql.DB, label string, level sql.IsolationLevel) {
	t0 := time.Now()
	tx := begin(db, level)
	_, err := tx.ExecContext(ctx, "update t1 set v = v + 1 where id = 1")
	took := time.Since(t0).Seconds()
	if err != nil {
		msg := firstLine(err)
		if fe, ok := fbsample.FbError(err); ok {
			msg = fmt.Sprintf("%s (gds %v)", strings.ReplaceAll(fbsample.ErrText(fe), "\n", ", "), fe.GDSCodes)
		}
		fmt.Printf("%-16s failed after %.3f s: %s\n", label, took, msg)
		tx.Rollback()
		return
	}
	fmt.Printf("%-16s granted after %.3f s\n", label, took)
	fbsample.Check(tx.Commit())
}

func main() {
	path := fbsample.DBPath("lock_manager")
	a, err := fbsample.Create(path)
	fbsample.Check(err)
	defer a.Close()
	b, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer b.Close()

	_, err = a.Exec("recreate table t1 (id int primary key, v int)")
	fbsample.Check(err)
	_, err = a.Exec("insert into t1 values (1, 0)")
	fbsample.Check(err)
	_, err = a.Exec("insert into t1 values (2, 0)")
	fbsample.Check(err)

	// The holder's uncommitted update makes row 1 contested: every probe
	// below ends up waiting on the holder transaction's LCK_tra lock.
	hold := begin(a, sql.LevelReadCommitted)
	_, err = hold.Exec("update t1 set v = 100 where id = 1")
	fbsample.Check(err)
	fmt.Println("holder: row 1 updated, uncommitted (LCK_tra held)")

	probe(context.Background(), b, "NO WAIT:", firebirdsql.LevelReadCommittedNoWait)

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	probe(ctx, b, "DEADLINE 3 s:", sql.LevelReadCommitted)
	cancel()

	// WAIT parks until the holder commits, 2 s from now; READ COMMITTED
	// rec_version then proceeds against the newest version and succeeds.
	released := make(chan struct{})
	go func() {
		time.Sleep(2 * time.Second)
		fbsample.Check(hold.Commit())
		fmt.Println("holder: committed (2 s later) -> lock released")
		close(released)
	}()
	probe(context.Background(), b, "WAIT:", sql.LevelReadCommitted)
	<-released

	// --- act two: a genuine wait-for cycle through LCK_tra locks ----------
	fmt.Println("building deadlock: A updates row 1, B updates row 2, then cross...")
	ta := begin(a, sql.LevelRepeatableRead) // SNAPSHOT WAIT
	tb := begin(b, sql.LevelRepeatableRead)
	_, err = ta.Exec("update t1 set v = v + 1 where id = 1")
	fbsample.Check(err)
	_, err = tb.Exec("update t1 set v = v + 1 where id = 2")
	fbsample.Check(err)

	t0 := time.Now()
	aDone := make(chan bool, 1) // true when A was the victim
	go func() {
		if _, err := ta.Exec("update t1 set v = v + 1 where id = 2"); err != nil {
			fmt.Printf("deadlock: A failed after %.1f s: %s\n", time.Since(t0).Seconds(), firstLine(err))
			ta.Rollback() // the victim's transaction still holds its locks: free B
			aDone <- true
			return
		}
		aDone <- false
	}()
	time.Sleep(300 * time.Millisecond)
	_, err = tb.Exec("update t1 set v = v + 1 where id = 1")
	if err != nil {
		fmt.Printf("deadlock: B failed after %.1f s: %s\n", time.Since(t0).Seconds(), firstLine(err))
		tb.Rollback() // free A
	} else {
		fmt.Printf("deadlock: B's update proceeded after %.1f s (A was the victim)\n", time.Since(t0).Seconds())
	}
	if aVictim := <-aDone; !aVictim {
		ta.Rollback()
	}
	if err == nil {
		tb.Rollback()
	}
	fmt.Println("the wait is DeadlockTimeout (10 s default): the cycle sat undetected until the scan.")
}
