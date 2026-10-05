// extensibility - calling native code through SQL: UDR end to end (Go twin
// of ../../cpp/extensibility.cpp; see ../../../extensibility.md).
//
// The shipped example UDR module (plugins/udr/libudrcpp_example.so) is
// bound to SQL names with EXTERNAL NAME '<module>!<entry>' ENGINE udr and
// called like any other procedure/function; then RDB$PROCEDURES /
// RDB$FUNCTIONS show the binding as metadata and RDB$CONFIG names the
// plugin filling each role.  Every seam here lives on the server, so a
// pure-Go wire client loses nothing: the DDL is db.Exec, the selectable
// procedure is a plain rows.Next loop, and the listing headers come from
// rows.Columns() - database/sql's view of the statement's output metadata.
//
// Run:  go run ./extensibility [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// table prints an isql-style listing, column names from rows.Columns().
func table(db *sql.DB, query string) {
	rs, err := db.Query(query)
	fbsample.Check(err)
	defer rs.Close()
	names, _ := rs.Columns()
	widths := make([]int, len(names))
	for i, n := range names {
		widths[i] = len(n)
	}
	var cells [][]string
	vals := make([]any, len(names))
	ptrs := make([]any, len(names))
	for i := range vals {
		ptrs[i] = &vals[i]
	}
	for rs.Next() {
		fbsample.Check(rs.Scan(ptrs...))
		row := make([]string, len(names))
		for i, v := range vals {
			if v != nil {
				row[i] = strings.TrimSpace(fbsample.Text(v))
			}
			widths[i] = max(widths[i], len(row[i]))
		}
		cells = append(cells, row)
	}
	fbsample.Check(rs.Err())
	line := func(vals []string) {
		parts := make([]string, len(vals))
		for i, v := range vals {
			parts[i] = fmt.Sprintf("%-*s", widths[i], v)
		}
		fmt.Println(strings.TrimRight(strings.Join(parts, " "), " "))
	}
	line(names)
	dashes := make([]string, len(names))
	for i, w := range widths {
		dashes[i] = strings.Repeat("-", w)
	}
	line(dashes)
	for _, c := range cells {
		line(c)
	}
}

func main() {
	db, err := fbsample.Create(fbsample.DBPath("extensibility"))
	fbsample.Check(err)
	defer db.Close()

	// 1. Bind SQL names to entry points in the shipped native module.
	_, err = db.Exec(`recreate procedure gen_rows (start_n integer not null,
	                                               end_n integer not null)
	    returns (n integer not null)
	    external name 'udrcpp_example!gen_rows' engine udr`)
	fbsample.Check(err)
	_, err = db.Exec(`recreate function sum_args (n1 integer, n2 integer, n3 integer)
	    returns integer
	    external name 'udrcpp_example!sum_args' engine udr`)
	fbsample.Check(err)

	// 2. Call them: native C++ running inside the server, plain SQL here.
	fmt.Println("select n from gen_rows(1, 5):")
	table(db, "select n from gen_rows(1, 5)")
	sum, err := fbsample.Scalar(db, "select sum_args(19, 20, 3) from rdb$database")
	fbsample.Check(err)
	fmt.Println("\nselect sum_args(19, 20, 3): ", fbsample.Text(sum))

	// 3. The binding is ordinary metadata...
	fmt.Println("\nexternal routines recorded in the system tables:")
	rows, err := fbsample.Rows(db, `
	    select trim(rdb$procedure_name) || '  ->  ' || trim(rdb$entrypoint)
	           || '  (engine ' || trim(rdb$engine_name) || ')'
	      from rdb$procedures where rdb$engine_name = 'UDR'
	    union all
	    select trim(rdb$function_name) || '  ->  ' || trim(rdb$entrypoint)
	           || '  (engine ' || trim(rdb$engine_name) || ')'
	      from rdb$functions where rdb$engine_name = 'UDR'`)
	fbsample.Check(err)
	for _, r := range rows {
		fmt.Println(fbsample.Text(r[0]))
	}

	// 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG.
	fmt.Println("\nplugins filling each role (rdb$config):")
	table(db, `select rdb$config_name, rdb$config_value from rdb$config
	            where rdb$config_name in ('Providers', 'AuthServer', 'UserManager',
	                  'WireCryptPlugin', 'TracePlugin', 'DefaultProfilerPlugin')
	            order by rdb$config_id`)
	fmt.Println("\ndone.")
}
