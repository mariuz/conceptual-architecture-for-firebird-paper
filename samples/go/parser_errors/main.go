// parser_errors - Firebird's SQL parser seen from the client (Go twin of
// ../../cpp/parser_errors.cpp; see ../../../grammar-and-parser.md).
//
// Six strings go to the parser: a `?` placeholder, FIRST in two grammatical
// roles (row-limit clause and plain column name), two syntax errors
// carrying the offending token's line/column, and a semantic error whose
// position survives past the parse.  db.Prepare is a genuine prepare-only
// step here - firebirdsql sends op_prepare_statement at once, so a parse
// error surfaces before anything executes - but the driver publishes none
// of what the prepare response describes: its driver.Stmt.NumInput()
// returns -1 and there is no statement type or input descriptor.  So the
// sample executes the good statements (the `?` bound to 2) and reads the
// output columns from rows.ColumnTypes(): name, the driver's type name and
// length.  A failed prepare returns *firebirdsql.FbError with the
// formatted status vector, SQLCode and the raw GDSCodes, so syntax
// (isc_dsql_token_unk_err) and semantic (isc_dsql_field_err) failures are
// told apart programmatically.
//
// Read-only against the stock employee database.
//
// Run:  go run ./parser_errors
package main

import (
	"database/sql"
	"fmt"
	"slices"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const (
	iscDsqlFieldErr    = 335544578 // "Column unknown"
	iscDsqlTokenUnkErr = 335544634 // "Token unknown"
)

// tryPrepare feeds one string to the parser; on success it executes it
// and reports the result columns, on failure the classified error.
func tryPrepare(db *sql.DB, query string, args ...any) {
	fmt.Println("----", query)
	stmt, err := db.Prepare(query)
	if err != nil {
		kind := "?"
		if fe, ok := fbsample.FbError(err); ok {
			switch {
			case slices.Contains(fe.GDSCodes, iscDsqlTokenUnkErr):
				kind = "syntax"
			case slices.Contains(fe.GDSCodes, iscDsqlFieldErr):
				kind = "semantic"
			}
			fmt.Printf("  prepare failed (%s; sqlcode %d):\n", kind, fe.SQLCode)
		} else {
			fmt.Println("  prepare failed:")
		}
		fmt.Println(fbsample.ErrText(err))
		return
	}
	defer stmt.Close()
	rs, err := stmt.Query(args...)
	fbsample.Check(err)
	defer rs.Close()
	cols, err := rs.ColumnTypes()
	fbsample.Check(err)
	fmt.Printf("  parsed OK: bound args=%d, output columns=%d\n", len(args), len(cols))
	vals := make([]any, len(cols))
	ptrs := make([]any, len(cols))
	for i := range vals {
		ptrs[i] = &vals[i]
	}
	if rs.Next() {
		fbsample.Check(rs.Scan(ptrs...))
	}
	for i, c := range cols {
		n, _ := c.Length()
		fmt.Printf("    column %d: %s %s(%d) = %s\n", i, c.Name(), c.DatabaseTypeName(), n,
			fbsample.Text(vals[i]))
	}
}

func main() {
	db, err := fbsample.Employee()
	fbsample.Check(err)
	defer db.Close()

	// 1. Dynamic SQL: the `?` becomes a parameter (bound to 2 here).
	tryPrepare(db, "SELECT first_name FROM employee WHERE emp_no = ?", 2)

	// 2. One token, two grammatical roles: FIRST as row-limit clause...
	tryPrepare(db, "SELECT FIRST 1 emp_no FROM employee")
	// ...and FIRST as an ordinary identifier (non-reserved keyword).
	tryPrepare(db, "SELECT first FROM (SELECT 1 AS first FROM rdb$database)")

	// 3. Syntax errors with token position.
	tryPrepare(db, "SELEC 1 FROM rdb$database")
	tryPrepare(db, "SELECT emp_no\nFROM employee\nWHERE ORDER BY 1")

	// 4. Semantic error - still carries line/column.
	tryPrepare(db, "SELECT frst_name\nFROM employee")
	fmt.Println("done.")
}
