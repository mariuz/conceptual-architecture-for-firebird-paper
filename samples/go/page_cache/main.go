// page_cache - the hot-page ping-pong against the SuperServer's one shared
// page cache, refereed by MON$IO_STATS (Go twin of ../../cpp/page_cache.cpp;
// see ../../../page-cache-coherency.md).
//
// Rows 1 and 2 share one data page; two workers each commit 300 updates to
// their own row, then report their attachment's page fetches, reads and
// writes.  The C++ twin runs this twice - once against the server's shared
// cache, once as two EMBEDDED engines with private caches.  firebirdsql is a
// pure wire-protocol client: there is no engine to load in-process, so
// phase 2 is out of reach, exactly as for node-firebird, and this twin is
// phase 1 only.  The two workers are goroutines rather than processes - each
// one its own *sql.DB, i.e. its own attachment, so to the server they are
// two clients hammering one page through one cache.
//
// Run:  go run ./page_cache [database]
package main

import (
	"fmt"
	"sync"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const rounds = 300

func worker(path string, row int) string {
	db, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer db.Close()
	for i := 0; i < rounds; i++ {
		tx, err := db.Begin()
		fbsample.Check(err)
		_, err = tx.Exec("update t set v = v + 1 where id = ?", row)
		fbsample.Check(err)
		fbsample.Check(tx.Commit())
	}
	var fetches, reads, writes int64
	fbsample.Check(db.QueryRow(
		"select MON$PAGE_FETCHES, MON$PAGE_READS, MON$PAGE_WRITES "+
			"from MON$IO_STATS join MON$ATTACHMENTS using (MON$STAT_ID) "+
			"where MON$ATTACHMENT_ID = CURRENT_CONNECTION").Scan(&fetches, &reads, &writes))
	return fmt.Sprintf("  worker row %d: %d commits | page fetches=%-6d reads=%-4d writes=%d",
		row, rounds, fetches, reads, writes)
}

func main() {
	path := fbsample.DBPath("page_cache")
	setup, err := fbsample.Create(path)
	fbsample.Check(err)
	for _, q := range []string{
		"recreate table t (id int primary key, v int)",
		"insert into t values (1, 0)",
		"insert into t values (2, 0)",
	} {
		_, err = setup.Exec(q)
		fbsample.Check(err)
	}
	setup.Close()

	fmt.Println("phase 1: two client attachments, ONE SuperServer shared cache")
	var wg sync.WaitGroup
	report := make([]string, 2)
	for i := range report {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			report[i] = worker(path, i+1)
		}(i)
	}
	wg.Wait()
	for _, line := range report {
		fmt.Println(line)
	}

	check, err := fbsample.Attach(path)
	fbsample.Check(err)
	defer check.Close()
	rows, err := fbsample.Rows(check, "select id, v from t order by id")
	fbsample.Check(err)
	for _, r := range rows {
		fmt.Printf("  final: id=%v v=%v (expected %d)\n", r[0], r[1], rounds)
	}
	fmt.Println("phase 2 (two embedded engines, private caches) needs an in-process engine:" +
		"\n  a pure wire-protocol driver cannot be one - see the C++, Rust, Pascal and Python twins.")
}
