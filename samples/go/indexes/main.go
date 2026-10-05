// indexes - one B-tree, many variants (Go twin of ../../cpp/indexes.cpp;
// see ../../../indexing-and-full-text-search.md).
//
// Builds a 3,000-row table with a descending, an expression (COMPUTED BY),
// a partial (WHERE) and a plain index, then shows the optimizer's access
// path for five queries: expression index, partial index, descending
// navigation, an OR bitmap-combining two indexes, and CONTAINING falling
// to NATURAL.  firebirdsql has no plan accessor (it defines
// isc_info_sql_get_plan but never requests it), so - like the rsfbclient
// twin - the sample asks the engine instead: Firebird 6's RDB$SQL.EXPLAIN
// table function returns the detailed access path as rows.  The Go
// difference is that the statement text travels as a bound parameter,
// RDB$SQL.EXPLAIN(?), so no quote-doubling is needed.
//
// Run:  go run ./indexes [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// plan prints the detailed access path of query, indented by LEVEL.
func plan(db *sql.DB, query string) {
	fmt.Println(query)
	rows, err := fbsample.Rows(db, `select "LEVEL", cast(access_path as varchar(512))
	                                   from rdb$sql.explain(?) order by plan_line`, query)
	fbsample.Check(err)
	for _, r := range rows {
		level := r[0].(int64)
		path := "(no access path)"
		if r[1] != nil {
			path = fbsample.Text(r[1])
		}
		// One record source per row; its ACCESS_PATH may span several
		// lines (a table's bitmap subtree), each indented by LEVEL.
		indent := strings.Repeat("    ", int(level))
		for _, line := range strings.Split(strings.TrimRight(path, "\n"), "\n") {
			fmt.Println(indent + line)
		}
	}
	fmt.Println()
}

func main() {
	db, err := fbsample.Create(fbsample.DBPath("indexes"))
	fbsample.Check(err)
	defer db.Close()

	exec := func(q string) {
		_, err := db.Exec(q)
		fbsample.Check(err)
	}
	exec("recreate table doc (id integer, title varchar(60), status varchar(10), num integer)")
	exec(`execute block as declare i integer = 0; begin
	        while (i < 3000) do begin
	          insert into doc values (:i, 'Title ' || :i,
	            iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100));
	          i = i + 1;
	        end
	      end`)
	exec("create descending index doc_id_desc on doc (id)")
	exec("create index doc_upper_title on doc computed by (upper(title))")
	exec("create index doc_active on doc (status) where status = 'active'")
	exec("create index doc_num on doc (num)")
	fmt.Println("3000 rows; indexes: descending, expression, partial, plain")
	fmt.Println()

	plan(db, "select id from doc where upper(title) = 'TITLE 5'")
	plan(db, "select id from doc where status = 'active'")
	plan(db, "select first 1 id from doc order by id desc")
	plan(db, "select id from doc where num = 42 or id = 7")
	plan(db, "select id from doc where title containing 'itle 12'")

	n, err := fbsample.Scalar(db, "select count(*) from doc where title containing 'itle 12'")
	fbsample.Check(err)
	fmt.Println("CONTAINING is correct but unindexed: matched", fbsample.Text(n),
		"rows by scanning all 3000")
	fmt.Println("done.")
}
