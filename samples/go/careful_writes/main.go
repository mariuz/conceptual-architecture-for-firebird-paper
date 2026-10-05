// careful_writes - kill a writer mid-transaction, then count (Go twin of
// ../../cpp/careful_writes.cpp; see ../../../careful-writes-and-crash-safety.md).
//
// The C++ twin attaches by plain path so the EMBEDDED engine runs in the
// process it SIGKILLs - a genuine engine crash.  firebirdsql is a pure
// wire-protocol client with no embedded mode, so (like node-firebird) it
// cannot put the engine in harm's way: what dies here is the CLIENT.  The
// parent re-runs its own binary as a writer that commits a marker row and
// then pushes 10,000-row EXECUTE BLOCK inserts in one transaction it never
// commits; the parent watches the .fdb grow (the server is flushing pages
// of uncommitted work), SIGKILLs the writer, re-attaches and counts.  The
// server sees a severed connection and the dead transaction's record
// versions are simply never visible - the reader-side half of the same
// guarantee: committed survives, uncommitted vanishes, nothing replays.
//
// Run:  go run ./careful_writes [database]
package main

import (
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func size(path string) int64 {
	if st, err := os.Stat(path); err == nil {
		return st.Size()
	}
	return -1
}

// writer: marker row committed, then uncommitted bulk inserts until killed.
func writer(path string) {
	db, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer db.Close()
	_, err = db.Exec("insert into cw values (1, 'committed-marker')") // autocommit
	fbsample.Check(err)
	fmt.Printf("[writer %d] marker row committed\n", os.Getpid())

	tx, err := db.Begin() // never committed: the crash victim
	fbsample.Check(err)
	for batch := 0; ; batch++ {
		_, err = tx.Exec(fmt.Sprintf(`execute block as declare i int = 0; begin
		  while (i < 10000) do begin
		    insert into cw values (%d + :i, 'uncommitted'); i = i + 1;
		  end
		end`, 1000+batch*10000))
		fbsample.Check(err)
	}
}

func main() {
	if len(os.Args) > 2 && os.Args[1] == "--writer" {
		writer(os.Args[2])
		return
	}
	path := fbsample.DBPath("careful_writes")

	db, err := fbsample.Recreate(path) // fresh database
	fbsample.Check(err)
	_, err = db.Exec("create table cw (id int, tag varchar(30))")
	fbsample.Check(err)
	db.Close()

	// 1. Spawn the writer: this same binary, a separate CLIENT process.
	self, err := os.Executable()
	fbsample.Check(err)
	cmd := exec.Command(self, "--writer", path)
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	fbsample.Check(cmd.Start())

	// 2. Wait until the server has flushed ~2 MB of the uncommitted
	//    transaction's pages (stat needs no read rights on the file), then
	//    SIGKILL the client mid-transaction.
	base := int64(-1)
	for i := 0; i < 1200; i++ {
		time.Sleep(50 * time.Millisecond)
		sz := size(path)
		if base < 0 && sz > 0 {
			base = sz
		}
		if base > 0 && sz > base+2*1024*1024 {
			fmt.Printf("file grew %d -> %d bytes; SIGKILL to writer pid %d (a client, not the engine)\n",
				base, sz, cmd.Process.Pid)
			break
		}
	}
	cmd.Process.Signal(syscall.SIGKILL)
	cmd.Wait()

	// 3. Re-attach and count: no recovery step exists to run.
	t0 := time.Now()
	db, err = fbsample.Attach(path)
	fbsample.Check(err)
	defer db.Close()
	var committed, uncommitted int
	fbsample.Check(db.QueryRow("select count(*) from cw where tag = 'committed-marker'").Scan(&committed))
	fbsample.Check(db.QueryRow("select count(*) from cw where tag = 'uncommitted'").Scan(&uncommitted))
	fmt.Printf("re-attach + both counts took %d ms\n", time.Since(t0).Milliseconds())
	fmt.Printf("committed marker rows : %d   <- durable\n", committed)
	fmt.Printf("uncommitted rows      : %d   <- gone with the killed writer\n", uncommitted)
}
