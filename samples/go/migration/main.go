// migration - the type-mapping table made concrete (Go twin of
// ../../cpp/migration.cpp; see ../../../migration-and-interoperability.md).
//
// A probe table with the types migrations trip over - INT128,
// NUMERIC(38,8), DECFLOAT(34), TIMESTAMP WITH TIME ZONE, BOOLEAN, CHAR(16)
// OCTETS (UUID) - inspected the three ways a migration tool sees it: the
// DESCRIBED metadata, the NATIVE face (what Go type each column arrives
// as) and the TEXT face (server-side CAST to VARCHAR, the universal
// fallback).  firebirdsql publishes the description through
// database/sql's rows.ColumnTypes(): its own type name for the wire code
// (INT128, DECFLOAT, TIMESTAMP WITH TIMEZONE...), length, precision/scale
// and the Go ScanType - no raw SQL_* number, which stays internal.  Being
// a pure-Go wire decoder it reaches the modern types natively: INT128,
// NUMERIC(38,8) and DECFLOAT(34) arrive as exact decimal strings,
// TIMESTAMP WITH TIME ZONE as a time.Time in the named zone.  The one gap
// is node-firebird's: CHAR(16) OCTETS fails with "Malformed string" on a
// charset=UTF8 attachment, because the driver's output BLR declares CHAR
// columns as plain blr_text (attachment charset) and the server then
// transliterates OCTETS -> UTF8; a charset=NONE attachment gets the raw
// []byte.
//
// Run:  go run ./migration [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

var columns = []string{"C_INT128", "C_NUM", "C_DEC", "C_TSTZ", "C_BOOL", "C_UUID", "C_VC"}

// native fetches one column and prints the Go type it arrives as.
func native(db *sql.DB, col string) {
	var v any
	if err := db.QueryRow("SELECT " + col + " FROM TYPE_PROBE").Scan(&v); err != nil {
		fmt.Printf("  %-8s -> ERROR: %s\n", col, strings.ReplaceAll(fbsample.ErrText(err), "\n", " / "))
		return
	}
	shown := fmt.Sprint(v)
	switch x := v.(type) {
	case time.Time:
		shown = fmt.Sprintf("%s [%s]", x.Format("2006-01-02 15:04:05 -07:00"), x.Location())
	case []byte:
		if len(x) == 16 {
			shown = fmt.Sprintf("%x-%x-%x-%x-%x", x[0:4], x[4:6], x[6:8], x[8:10], x[10:16])
		} else {
			shown = string(x)
		}
	}
	fmt.Printf("  %-8s -> %-10T %s\n", col, v, shown)
}

func main() {
	db, err := fbsample.Create(fbsample.DBPath("migration"))
	fbsample.Check(err)
	defer db.Close()

	_, err = db.Exec(`RECREATE TABLE TYPE_PROBE (
	                    C_INT128 INT128,
	                    C_NUM    NUMERIC(38,8),
	                    C_DEC    DECFLOAT(34),
	                    C_TSTZ   TIMESTAMP WITH TIME ZONE,
	                    C_BOOL   BOOLEAN,
	                    C_UUID   CHAR(16) CHARACTER SET OCTETS,
	                    C_VC     VARCHAR(20))`)
	fbsample.Check(err)
	_, err = db.Exec(`INSERT INTO TYPE_PROBE VALUES (
	                    170141183460469231731687303715884105727,
	                    123456789012345678901234567890.12345678,
	                    1.234567890123456789012345678901234E+10,
	                    TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',
	                    TRUE, GEN_UUID(), 'naïve ütf8 text')`)
	fbsample.Check(err)

	// -- 1. what DESCRIBE tells a driver -----------------------------------
	rs, err := db.Query("SELECT * FROM TYPE_PROBE")
	fbsample.Check(err)
	cols, err := rs.ColumnTypes()
	fbsample.Check(err)
	fmt.Println("described output metadata of SELECT * FROM TYPE_PROBE (rows.ColumnTypes):")
	fmt.Println()
	fmt.Printf("%-8s %-24s %6s %5s  %s\n", "column", "DatabaseTypeName", "length", "scale", "ScanType")
	for _, c := range cols {
		n, _ := c.Length()
		_, scale, _ := c.DecimalSize()
		fmt.Printf("%-8s %-24s %6d %5d  %v\n", c.Name(), c.DatabaseTypeName(), n, scale, c.ScanType())
	}

	rs.Close()

	// -- 2. the native face: Go values, no text in between -----------------
	// One column per query, so a column the driver cannot fetch fails alone.
	fmt.Println("\nsame row fetched natively, column by column (charset=UTF8 attachment):")
	fmt.Println()
	for _, c := range columns {
		native(db, c)
	}
	// The OCTETS failure is the connection charset at work: firebirdsql's
	// output BLR declares CHAR columns as plain blr_text, i.e. "in the
	// attachment charset", so the server transliterates OCTETS -> UTF8 and
	// rejects the bytes.  A charset=NONE attachment gets them raw.
	raw, err := fbsample.Attach(fbsample.DBPath("migration"), "charset=NONE")
	fbsample.Check(err)
	fmt.Println("\nthe UUID again, on a charset=NONE attachment:")
	native(raw, "C_UUID")
	raw.Close()

	// -- 3. the text face: engine-rendered strings, the ETL fallback -------
	fmt.Println("\nthe text face (CAST ... AS VARCHAR server-side):")
	fmt.Println()
	casts := make([]string, 5)
	for i, c := range columns[:5] {
		casts[i] = fmt.Sprintf("CAST(%s AS VARCHAR(64))", c)
	}
	rows, err := fbsample.Rows(db, "SELECT "+strings.Join(casts, ", ")+
		", UUID_TO_CHAR(C_UUID), C_VC FROM TYPE_PROBE")
	fbsample.Check(err)
	for i, v := range rows[0] {
		fmt.Printf("  %-8s = %s\n", columns[i], fbsample.Text(v))
	}
	fmt.Println("\ndone.")
}
