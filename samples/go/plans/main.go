// plans - watching the cost-based optimizer decide (Go twin of
// ../../cpp/plans.cpp; see ../../../query-optimizer-and-execution.md).
//
// The same five experiments: the plan flip from Full Scan to Bitmap + Index
// Range Scan after CREATE INDEX, a unique-index PK lookup, SORT over a
// nested loop, and the indexless equi-join that becomes a Hash Join.
//
// firebirdsql never asks the wire for plan info items (it defines
// isc_info_sql_get_plan but its op_info_sql is internal), so - like the
// node-firebird and rsfbclient twins - it takes Firebird 6's SQL-level route,
// the RDB$SQL.EXPLAIN table function, which prepares the statement
// server-side and returns the record-source tree as rows, never executing
// it.  The legacy PLAN (...) one-liner stays out of reach.  Two Go details:
// ACCESS_PATH is a BLOB, and the driver fetches blob contents itself, so it
// scans straight into a []byte with no CAST; and the rows also carry the
// optimizer's CARDINALITY estimate per operator, printed alongside.
//
// Run:  go run ./plans [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// explain prints one statement's record-source tree, indented by LEVEL.
func explain(db *sql.DB, query string) {
	fmt.Println("==", query)
	rows, err := db.Query("SELECT \"LEVEL\", cardinality, access_path "+
		"FROM rdb$sql.explain(?) ORDER BY plan_line", query)
	fbsample.Check(err)
	defer rows.Close()
	for rows.Next() {
		var level int64
		var card sql.NullFloat64
		var path []byte // a BLOB, fetched by the driver
		fbsample.Check(rows.Scan(&level, &card, &path))
		pad := "   " + strings.Repeat("    ", int(level))
		text := strings.ReplaceAll(strings.TrimRight(string(path), "\n "), "\n", "\n"+pad)
		est := ""
		if card.Valid {
			est = fmt.Sprintf("   [est. %.0f rows]", card.Float64)
		}
		fmt.Printf("%s%s%s\n", pad, text, est)
	}
	fbsample.Check(rows.Err())
	fmt.Println()
}

func main() {
	db, err := fbsample.Create(fbsample.DBPath("plans"))
	fbsample.Check(err)
	defer db.Close()

	// -- Build the schema: 20 departments, 2000 employees. ------------------
	for _, q := range []string{
		"RECREATE TABLE dept (id INT NOT NULL PRIMARY KEY, name VARCHAR(20))",
		"RECREATE TABLE emp (id INT NOT NULL PRIMARY KEY," +
			" dept_id INT, salary INT, name VARCHAR(20))",
		`EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN
  WHILE (i <= 20) DO BEGIN
    INSERT INTO dept VALUES (:i, 'dept ' || :i); i = i + 1;
  END
  i = 1;
  WHILE (i <= 2000) DO BEGIN
    INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,
        1000 + MOD(:i * 37, 500), 'emp ' || :i); i = i + 1;
  END
END`,
	} {
		_, err := db.Exec(q)
		fbsample.Check(err)
	}

	// -- 1. No index on dept_id yet: the full scan is the only path. --------
	explain(db, "SELECT name FROM emp WHERE dept_id = 5")

	// -- 2. Create the index; the same text now compiles differently. -------
	_, err = db.Exec("CREATE INDEX emp_dept ON emp (dept_id)")
	fbsample.Check(err)
	fmt.Println("-- CREATE INDEX emp_dept ON emp (dept_id) --")
	fmt.Println()
	explain(db, "SELECT name FROM emp WHERE dept_id = 5")

	// -- 3. PK equality: unique index, nothing cheaper than one row. --------
	explain(db, "SELECT name FROM emp WHERE id = 42")

	// -- 4. Join + ORDER BY: SORT over a nested loop with the index. --------
	explain(db, "SELECT e.name, d.name FROM emp e"+
		" JOIN dept d ON e.dept_id = d.id ORDER BY e.salary")

	// -- 5. Equi-join with no usable index on either side: hash join. -------
	explain(db, "SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary")
	fmt.Println("done.")
}
