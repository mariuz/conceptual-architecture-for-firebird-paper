// schemas - schemas and the search path (Go twin of ../../cpp/schemas.cpp;
// see ../../../schemas-and-name-resolution.md).
//
// The five demonstrations: RDB$SCHEMAS and the default path; one
// unqualified SELECT resolving to PUBLIC.CUSTOMERS or APP.CUSTOMERS as SET
// SEARCH_PATH changes; SYSTEM auto-appended; a procedure created while APP
// leads the path keeping APP.CUSTOMERS after the session flips to PUBLIC
// (RDB$DEPENDENCIES records it); and the schema-qualified plan.
//
// Resolution is server-side, so nothing needs driver support except the
// plan: firebirdsql has no plan API (it never asks for
// isc_info_sql_get_plan), so - like the rsfbclient twin - step 5 asks
// Firebird 6's RDB$SQL.EXPLAIN for the access path, from SQL.  Every Exec
// outside a transaction is the driver's autocommit (COMMIT RETAINING), so
// the path surviving those commits shows it is attachment state, and
// database/sql's pool is pinned to one connection so every statement
// really runs on the same attachment - the session the path belongs to.
//
// Run:  go run ./schemas [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const (
	pathQuery = "select rdb$get_context('SYSTEM', 'SEARCH_PATH') from rdb$database"
	which     = "select origin from customers"
)

func exec(db *sql.DB, stmt string) {
	_, err := db.Exec(stmt)
	fbsample.Check(err)
}

func scalar(db *sql.DB, query string) string {
	v, err := fbsample.Scalar(db, query)
	fbsample.Check(err)
	return strings.TrimSpace(fbsample.Text(v))
}

func main() {
	db, err := fbsample.Create(fbsample.DBPath("schemas"))
	fbsample.Check(err)
	defer db.Close()

	// -- Idempotent cleanup + setup.
	for _, s := range []string{"drop procedure app.which_one", "drop table public.customers",
		"drop table app.customers", "drop schema app"} {
		db.Exec(s) // a missing object is fine
	}
	exec(db, "create schema app")
	exec(db, "create table public.customers (id int, origin varchar(20))")
	exec(db, "create table app.customers    (id int, origin varchar(20))")
	exec(db, "insert into public.customers values (1, 'from PUBLIC')")
	exec(db, "insert into app.customers    values (2, 'from APP')")

	// -- 1. The catalog and the default path.
	rows, err := fbsample.Rows(db, "select trim(rdb$schema_name) from rdb$schemas order by 1")
	fbsample.Check(err)
	names := []string{}
	for _, r := range rows {
		names = append(names, fbsample.Text(r[0]))
	}
	fmt.Println("schemas in RDB$SCHEMAS      :", strings.Join(names, "  "))
	fmt.Println("default search path         :", scalar(db, pathQuery))

	// -- 2. Same statement, two resolutions.
	fmt.Println("\nSELECT ORIGIN FROM CUSTOMERS, as the path changes:")
	fmt.Println("  path PUBLIC,SYSTEM        ->", scalar(db, which))
	exec(db, "set search_path to app, public")
	fmt.Println("  path APP,PUBLIC           ->", scalar(db, which))

	// -- 3. SYSTEM can be moved but not removed.
	exec(db, "set search_path to app")
	fmt.Println("\nSET SEARCH_PATH TO APP      ->", scalar(db, pathQuery), "  (SYSTEM auto-appended)")

	// -- 4. Stored code binds its own schema, not the caller's path.
	exec(db, "set search_path to app, public")
	exec(db, "create procedure which_one returns (src varchar(20)) as "+
		"begin select origin from customers into :src; suspend; end")
	fmt.Println("\nprocedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)")
	exec(db, "set search_path to public")
	fmt.Println("  after SET SEARCH_PATH TO PUBLIC:")
	fmt.Println("    direct SELECT ... FROM CUSTOMERS ->", scalar(db, which))
	fmt.Println("    SELECT SRC FROM APP.WHICH_ONE    ->", scalar(db, "select src from app.which_one"),
		"  <- unmoved")
	fmt.Println("    RDB$DEPENDENCIES records         ->", scalar(db,
		"select trim(rdb$depended_on_schema_name) || '.' || trim(rdb$depended_on_name)"+
			"  from rdb$dependencies where rdb$dependent_name = 'WHICH_ONE'"))

	// -- 5. Plans are schema-qualified (RDB$SQL.EXPLAIN: no plan API here).
	fmt.Println("\nRDB$SQL.EXPLAIN('SELECT COUNT(*) FROM CUSTOMERS') with path PUBLIC:")
	plan, err := fbsample.Rows(db,
		"select plan_line, trim(schema_name) || '.' || trim(object_name),"+
			"       cast(access_path as varchar(300))"+
			"  from rdb$sql.explain('SELECT COUNT(*) FROM CUSTOMERS') order by plan_line")
	fbsample.Check(err)
	for _, r := range plan {
		obj := ""
		if r[1] != nil {
			obj = "<- " + fbsample.Text(r[1])
		}
		line := fmt.Sprintf("  %2s  %-40s %s", fbsample.Text(r[0]), strings.TrimSpace(fbsample.Text(r[2])), obj)
		fmt.Println(strings.TrimRight(line, " "))
	}
	fmt.Println("\ndone.")
}
