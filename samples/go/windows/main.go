// windows - window functions and modern aggregates, live (Go twin of
// ../../cpp/windows.cpp; see ../../../aggregate-and-window-functions.md).
//
// Recreates the document's six-row sales table and runs the flagship
// ranking/frame/LAG window query, FILTER + LISTAGG + STDDEV_POP,
// PERCENTILE_CONT with a hypothetical-set RANK(175) WITHIN GROUP, and a
// Firebird 6 frame with EXCLUDE CURRENT ROW.  Two firebirdsql specifics:
// scaled NUMERICs arrive as exact decimal strings (no float rounding),
// INT128 included - so the running SUM, which widens NUMERIC(10,2) to
// NUMERIC(20,2), needs no CAST here, unlike the Rust twin; and the driver
// has no plan API, so the window query's plan comes from Firebird 6's
// RDB$SQL.EXPLAIN table function, plain SQL, as in the Rust twin.
//
// Run:  go run ./windows [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// printTable renders a result set isql-style, headers = SELECT-list aliases.
func printTable(tx *sql.Tx, query string, args ...any) {
	rs, err := tx.Query(query, args...)
	fbsample.Check(err)
	defer rs.Close()
	cols, _ := rs.Columns()
	var table [][]string
	for rs.Next() {
		vals := make([]any, len(cols))
		ptrs := make([]any, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		fbsample.Check(rs.Scan(ptrs...))
		row := make([]string, len(cols))
		for i, v := range vals {
			row[i] = fbsample.Text(v)
		}
		table = append(table, row)
	}
	fbsample.Check(rs.Err())
	width := make([]int, len(cols))
	for i, c := range cols {
		width[i] = len(c)
		for _, r := range table {
			width[i] = max(width[i], len(r[i]))
		}
	}
	line := func(cells []string) {
		parts := make([]string, len(cells))
		for i, c := range cells {
			parts[i] = fmt.Sprintf("%-*s", width[i], c)
		}
		fmt.Println(strings.TrimRight(strings.Join(parts, " "), " "))
	}
	line(cols)
	dashes := make([]string, len(cols))
	for i := range cols {
		dashes[i] = strings.Repeat("-", width[i])
	}
	line(dashes)
	for _, r := range table {
		line(r)
	}
}

const winSQL = `SELECT region, amount,
  ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount) AS rn,
  RANK() OVER (ORDER BY amount DESC) AS overall_rank,
  SUM(amount) OVER (PARTITION BY region ORDER BY id
    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_total,
  LAG(amount) OVER (PARTITION BY region ORDER BY id) AS prev_amount
FROM sales`

func main() {
	db, err := fbsample.Create(fbsample.DBPath("windows"))
	fbsample.Check(err)
	defer db.Close()

	db.Exec("DROP TABLE sales") // first run: nothing to drop
	_, err = db.Exec("CREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(10), amount NUMERIC(10,2))")
	fbsample.Check(err)

	tx, err := db.Begin()
	fbsample.Check(err)
	defer tx.Rollback()
	ins, err := tx.Prepare("INSERT INTO sales VALUES (?, ?, ?)")
	fbsample.Check(err)
	for _, r := range []struct {
		id     int
		region string
		amount int
	}{{1, "East", 100}, {2, "East", 200}, {3, "East", 150}, {4, "West", 300}, {5, "West", 250}, {6, "West", 400}} {
		_, err = ins.Exec(r.id, r.region, r.amount)
		fbsample.Check(err)
	}
	ins.Close()

	// -- 1. The flagship window query: every row kept.
	fmt.Println("== window functions ==")
	printTable(tx, winSQL)

	// No getPlan in database/sql or this driver: ask the engine in SQL.
	fmt.Println("\nplan (RDB$SQL.EXPLAIN - firebirdsql has no plan API):")
	plan, err := fbsample.Rows(tx,
		`SELECT "LEVEL", ACCESS_PATH FROM RDB$SQL.EXPLAIN(?) ORDER BY PLAN_LINE`, winSQL)
	fbsample.Check(err)
	for _, p := range plan {
		level := p[0].(int64)
		for _, l := range strings.Split(strings.TrimRight(fbsample.Text(p[1]), "\n"), "\n") {
			fmt.Printf("  %s%s\n", strings.Repeat("  ", int(level)), l)
		}
	}

	// -- 2. Aggregates: FILTER, ordered LISTAGG, statistical.
	fmt.Println("\n== aggregates: FILTER / LISTAGG / STDDEV_POP ==")
	printTable(tx, `SELECT region, COUNT(*) AS n,
	  COUNT(*) FILTER (WHERE amount > 150) AS big_sales,
	  CAST(LISTAGG(amount, ',') WITHIN GROUP (ORDER BY amount) AS VARCHAR(60)) AS amounts,
	  CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev
	FROM sales GROUP BY region`)

	// -- 3. Ordered-set and hypothetical-set aggregates.
	fmt.Println("\n== PERCENTILE_CONT median / hypothetical RANK(175) ==")
	printTable(tx, `SELECT region,
	  PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median,
	  RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175
	FROM sales GROUP BY region`)

	// -- 4. FB6 frame exclusion: the neighbours' average.
	fmt.Println("\n== FB6 frame EXCLUDE CURRENT ROW (neighbours' average) ==")
	printTable(tx, `SELECT id, amount,
	  CAST(AVG(amount) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING
	    EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg
	FROM sales`)

	fbsample.Check(tx.Commit())
	fmt.Println("\ndone.")
}
