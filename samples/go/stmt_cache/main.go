// stmt_cache - the server's DSQL statement cache inferred from prepare
// timings (Go twin of ../../cpp/stmt_cache.cpp; see ../../../statement-cache.md).
//
// There is no monitoring view of the cache, so - like the document's own
// demonstrations - the sample times prepares of a statement that is heavy
// to *compile* (a six-way self-join: large join-order search) and is never
// executed; every loop is prepare + close only:
//
//	run 1  identical text               -> all but the first prepare hit
//	run 2  same SQL + i trailing spaces -> all miss: the key is the text verbatim
//	run 3  distinct literal each time   -> all miss, for comparison
//	run 4  identical text, each prepare preceded by an unrelated
//	       RECREATE TABLE + commit      -> all miss: a DDL commit purges the cache
//
// database/sql keeps no statement cache of its own and tx.Prepare is a real
// prepare-without-execute (op_allocate + op_prepare, metadata included);
// Stmt.Close sends op_free_statement(DSQL_drop).  The prepares run inside
// an explicit transaction, because in the driver's autocommit mode every
// Stmt.Close would also COMMIT RETAINING.  database/sql pins the one
// connection to that Tx while it is open, so run 4's DDL comes from a
// SECOND attachment - which makes the point sharper: the cache is
// per-database, and another attachment's DDL commit purges this one's.
//
// Run:  go run ./stmt_cache [database]
package main

import (
	"database/sql"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const (
	heavy = "SELECT COUNT(*) FROM t a" +
		" JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id" +
		" JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id" +
		" JOIN t f ON e.id = f.id WHERE a.id > 0"
	n = 100
)

// prepareOnce prepares and frees one statement; returns the elapsed time.
func prepareOnce(tx *sql.Tx, query string) time.Duration {
	t0 := time.Now()
	stmt, err := tx.Prepare(query)
	fbsample.Check(err)
	fbsample.Check(stmt.Close())
	return time.Since(t0)
}

func report(label string, total time.Duration, verdict string) {
	ms := float64(total.Microseconds()) / 1000
	fmt.Printf("%-29s %3d prepares: %6.1f ms  (%.2f ms/prepare) - %s\n", label, n, ms, ms/n, verdict)
}

func main() {
	path := fbsample.DBPath("stmt_cache")
	db, err := fbsample.Create(path)
	fbsample.Check(err)
	defer db.Close()
	ddl, err := fbsample.Attach(path) // second attachment, for run 4
	fbsample.Check(err)
	defer ddl.Close()

	_, err = db.Exec("RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)")
	fbsample.Check(err)
	_, err = db.Exec("EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO" +
		" BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END")
	fbsample.Check(err)

	tx, err := db.Begin()
	fbsample.Check(err)
	defer tx.Commit()
	prepareOnce(tx, heavy) // warm the cache with the exact text

	var total time.Duration
	for i := 0; i < n; i++ {
		total += prepareOnce(tx, heavy)
	}
	report("1. identical text", total, "hits")

	total = 0
	for i := 0; i < n; i++ {
		total += prepareOnce(tx, heavy+strings.Repeat(" ", i+1))
	}
	report("2. + i trailing spaces", total, "misses")

	total = 0
	for i := 0; i < n; i++ {
		total += prepareOnce(tx, strings.Replace(heavy, "> 0", "> "+strconv.Itoa(i), 1))
	}
	report("3. distinct literal", total, "misses")

	total = 0 // time only the prepares, not the DDL itself
	for i := 0; i < n; i++ {
		_, err := ddl.Exec("RECREATE TABLE unrelated (x INT)") // autocommit: purges the cache
		fbsample.Check(err)
		total += prepareOnce(tx, heavy)
	}
	report("4. identical text after DDL", total, "misses")
}
