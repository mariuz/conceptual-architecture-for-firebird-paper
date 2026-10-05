// backup - a gbak backup + restore round trip through the Services API
// (Go twin of ../../cpp/backup.cpp; see ../../../backup-and-recovery.md).
//
// Creates a scratch database with three rows, backs it up with
// isc_action_svc_backup (verbose) while the source attachment is still
// open - gbak reads through a snapshot - restores it with
// isc_action_svc_restore (replace) and proves the rows survived.  No gbak
// binary and no libfbclient: firebirdsql reimplements op_service_attach /
// op_service_start / op_service_info in Go, and its BackupManager builds the
// SPBs from option structs.  Two driver-specific shapes: each Backup /
// Restore call opens and closes its OWN service_mgr attachment (the C++
// twin reuses one), and gbak's isc_info_svc_line output arrives on a Go
// channel the caller owns - the operation runs in a goroutine, the main
// goroutine ranges over the lines, and the caller closes the channel.
//
// Run:  go run ./backup [database]
package main

import (
	"fmt"

	"github.com/nakagami/firebirdsql"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// stream runs one service operation and prints its verbose log as it comes.
func stream(op func(chan string) error) {
	lines := make(chan string)
	errc := make(chan error, 1)
	go func() {
		errc <- op(lines)
		close(lines) // the channel is the caller's to close
	}()
	for l := range lines {
		fmt.Println("  gbak>", l)
	}
	fbsample.Check(<-errc)
}

func main() {
	src := fbsample.DBPath("backup")
	fbk := fbsample.Scratch + "/backup_go.fbk" // server-side path
	restored := fbsample.Scratch + "/backup_go_restored.fdb"

	// -- 1. scratch source database (kept attached during the backup)
	db, err := fbsample.Create(src)
	fbsample.Check(err)
	defer db.Close()
	db.Exec("DROP TABLE BR_ITEMS") // first run: nothing to drop
	_, err = db.Exec("CREATE TABLE BR_ITEMS (ID INT NOT NULL PRIMARY KEY, NAME VARCHAR(30))")
	fbsample.Check(err)
	for i, name := range []string{"alpha", "beta", "gamma"} {
		_, err = db.Exec("INSERT INTO BR_ITEMS VALUES (?, ?)", i+1, name)
		fbsample.Check(err)
	}
	fmt.Println("source ready: BR_ITEMS with 3 rows")

	bm, err := firebirdsql.NewBackupManager(fbsample.ServiceAddr(), fbsample.User, fbsample.Password,
		fbsample.ServiceOptions())
	fbsample.Check(err)

	// -- 2. gbak backup through the service, verbose
	fmt.Printf("\n== backup: %s -> %s ==\n", src, fbk)
	stream(func(lines chan string) error {
		return bm.Backup(src, fbk, firebirdsql.NewBackupOptions(), lines)
	})

	// -- 3. gbak restore (replace)
	fmt.Printf("\n== restore: %s -> %s ==\n", fbk, restored)
	stream(func(lines chan string) error {
		return bm.Restore(fbk, restored, firebirdsql.NewRestoreOptions(firebirdsql.WithReplace()), lines)
	})

	// -- 4. prove the restored copy has the data
	rdb, err := fbsample.Attach(restored)
	fbsample.Check(err)
	defer rdb.Close()
	var n int
	var maxName string
	fbsample.Check(rdb.QueryRow("SELECT COUNT(*), MAX(NAME) FROM BR_ITEMS").Scan(&n, &maxName))
	fmt.Printf("\nrestored database says: %d rows, max name = %s\n", n, maxName)
	fmt.Println("done.")
}
