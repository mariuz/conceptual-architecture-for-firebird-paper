// gc_sweep - the record-version lifecycle seen through MON$RECORD_STATS
// (Go twin of ../../cpp/gc_sweep.cpp; see
// ../../../garbage-collection-and-sweep.md).
//
// Pins a SNAPSHOT, commits twelve updates under it, releases it, scans,
// then deletes - watching the database-level counters that are vio.cpp's
// events (MON$RECORD_IMGC = VIO_intermediate_gc, PURGES = purge(),
// EXPUNGES = expunge(), MON$BACKVERSION_READS = chain walks).
//
// The pin is database/sql's LevelRepeatableRead (isc_tpb_concurrency).
// The stump is where firebirdsql's fixed TPBs stop: there is no
// isc_tpb_no_auto_undo and no raw-TPB escape hatch, so - like the
// rsfbclient twin - the sample shows the absence instead: an ordinary
// rollback undoes its work in memory and is booked as committed in the
// TIP, and the OIT keeps moving.  What the pure-Go driver does carry is
// the sweep the C++ sample only recommends: MaintenanceManager.Sweep is
// the Services API's isc_action_svc_repair + isc_spb_rpr_sweep_db
// (gfix -sweep), run inside the server.
//
// Run:  go run ./gc_sweep [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"time"

	"github.com/nakagami/firebirdsql"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const selectVal = "select val from gctest where id = 1"

// inTx runs fn in its own READ COMMITTED transaction and commits it -
// MON$ tables are a stable snapshot per transaction, so every peek is fresh.
func inTx(db *sql.DB, fn func(tx *sql.Tx)) {
	tx, err := db.Begin()
	fbsample.Check(err)
	fn(tx)
	fbsample.Check(tx.Commit())
}

func exec(db *sql.DB, query string, args ...any) {
	inTx(db, func(tx *sql.Tx) {
		_, err := tx.Exec(query, args...)
		fbsample.Check(err)
	})
}

func showStats(db *sql.DB, label string) {
	inTx(db, func(tx *sql.Tx) {
		var upd, imgc, purges, expunges, back int64
		fbsample.Check(tx.QueryRow(`
		    select r.MON$RECORD_UPDATES, r.MON$RECORD_IMGC, r.MON$RECORD_PURGES,
		           r.MON$RECORD_EXPUNGES, r.MON$BACKVERSION_READS
		      from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)`).
			Scan(&upd, &imgc, &purges, &expunges, &back))
		fmt.Printf("%-34s upd=%-4d imgc=%-3d purges=%-3d expunges=%-3d backreads=%d\n",
			label, upd, imgc, purges, expunges, back)
	})
}

func showCounters(db *sql.DB, label string) {
	inTx(db, func(tx *sql.Tx) {
		var oit, oat, ost, next, interval int64
		fbsample.Check(tx.QueryRow(`
		    select MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$OLDEST_SNAPSHOT,
		           MON$NEXT_TRANSACTION, MON$SWEEP_INTERVAL from MON$DATABASE`).
			Scan(&oit, &oat, &ost, &next, &interval))
		fmt.Printf("%-34s OIT=%d OAT=%d OST=%d Next=%d (sweep interval %d)\n",
			label, oit, oat, ost, next, interval)
	})
}

func main() {
	path := fbsample.DBPath("gc_sweep")
	writer, err := fbsample.Recreate(path)
	fbsample.Check(err)
	defer writer.Close()
	pinner, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer pinner.Close()

	exec(writer, "create table gctest (id int primary key, val int)")
	exec(writer, "insert into gctest values (1, 0)")

	// 1. Pin a snapshot: while it lives, version 0 must survive.
	snap, err := pinner.BeginTx(context.Background(), &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
	fbsample.Check(err)
	var val int
	fbsample.Check(snap.QueryRow(selectVal).Scan(&val))
	fmt.Println("pinned SNAPSHOT reads val =", val)
	showStats(writer, "before updates:")

	// 2. Twelve committed updates -> twelve back versions... in theory.
	for i := 1; i <= 12; i++ {
		exec(writer, "update gctest set val = ? where id = 1", i)
	}
	showStats(writer, "after 12 updates (snapshot open):")
	fbsample.Check(snap.QueryRow(selectVal).Scan(&val))
	fmt.Println("pinned SNAPSHOT still reads val =", val)

	// 3. Release the snapshot; a scan now trips over the below-OST chain.
	fbsample.Check(snap.Commit())
	inTx(writer, func(tx *sql.Tx) {
		fbsample.Check(tx.QueryRow(selectVal).Scan(&val))
	})
	fmt.Println("snapshot released; new reader sees val =", val)
	time.Sleep(1500 * time.Millisecond)
	showStats(writer, "after release + scan + 1.5s:")

	// 4. A committed DELETE older than the OST is expunged, not purged.
	exec(writer, "delete from gctest where id = 1")
	inTx(writer, func(tx *sql.Tx) {
		var n int
		fbsample.Check(tx.QueryRow("select count(*) from gctest").Scan(&n)) // scan -> collect
	})
	time.Sleep(1500 * time.Millisecond)
	showStats(writer, "after DELETE + scan + 1.5s:")

	// 5. The stump needs isc_tpb_no_auto_undo, which no database/sql level
	//    maps to: an ordinary SNAPSHOT rollback undoes in memory and is
	//    booked as committed, so the OIT does not freeze.
	showCounters(writer, "header counters before rollback:")
	tx, err := writer.BeginTx(context.Background(), &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
	fbsample.Check(err)
	_, err = tx.Exec("insert into gctest values (2, 0)")
	fbsample.Check(err)
	fbsample.Check(tx.Rollback())
	showCounters(writer, "after (auto-undo) rollback:")
	// A stump would hold the OIT where it is; without one it keeps pace.
	showCounters(writer, "one transaction later:")

	// 6. The sweep, through the Services API (gfix -sweep's engine).
	mm, err := firebirdsql.NewMaintenanceManager(fbsample.ServiceAddr(), fbsample.User,
		fbsample.Password, fbsample.ServiceOptions())
	fbsample.Check(err)
	fbsample.Check(mm.Sweep(path))
	showCounters(writer, "after MaintenanceManager.Sweep():")
}
