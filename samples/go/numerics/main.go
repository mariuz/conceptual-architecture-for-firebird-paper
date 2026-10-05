// numerics - exact and approximate arithmetic as a Go client receives it
// (Go twin of ../../cpp/numerics.cpp; see
// ../../../numeric-and-precision-arithmetic.md).
//
// Four experiments: the residue of (0.1 + 0.2) - 0.3 in DOUBLE PRECISION vs
// DECFLOAT(34); NUMERIC(18,4) seen through the column metadata the driver
// decoded off the wire (INT64, scale -4); INT128 at 2^127-1 and one step
// beyond; the DECFLOAT Division_by_zero trap, then SET DECFLOAT TRAPS TO.
//
// firebirdsql's instructive difference: every exact type arrives as a
// decimal STRING the driver formatted itself from the wire integer and the
// scale - scaled NUMERIC, INT128 (math/big) and DECFLOAT (its own BID/DPD
// decoder, Infinity and NaN included).  No float64 in between, so the 2^53
// cent the JavaScript and Rust twins lose is kept here, and DECFLOAT and
// INT128 need no server-side CAST.  The other side: the raw message buffer
// is private, so the scaled integer is shown through database/sql's
// ColumnType metadata (DatabaseTypeName, DecimalSize) rather than as bytes.
//
// Run:  go run ./numerics [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func value(db *sql.DB, query string) string {
	v, err := fbsample.Scalar(db, query)
	fbsample.Check(err)
	return fmt.Sprintf("%v  (Go %T)", fbsample.Text(v), v)
}

func failure(db *sql.DB, query string) string {
	if _, err := fbsample.Scalar(db, query); err != nil {
		msg := strings.SplitN(fbsample.ErrText(err), "\n", 2)[0]
		if fe, ok := fbsample.FbError(err); ok {
			return fmt.Sprintf("%s (sqlcode %d, gds %v)", msg, fe.SQLCode, fe.GDSCodes)
		}
		return msg
	}
	return "BUG: no error"
}

func main() {
	db, err := fbsample.Create(fbsample.DBPath("numerics"))
	fbsample.Check(err)
	defer db.Close()

	// -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3.
	fmt.Println("(0.1+0.2)-0.3 in DOUBLE PRECISION :", value(db,
		"SELECT (CAST(0.1 AS DOUBLE PRECISION) + 0.2) - 0.3 FROM RDB$DATABASE"))
	fmt.Println("(0.1+0.2)-0.3 in DECFLOAT(34)     :", value(db,
		"SELECT (CAST(0.1 AS DECFLOAT(34)) + 0.2) - 0.3 FROM RDB$DATABASE"))
	fmt.Println()

	// -- 2. NUMERIC(18,4): a scaled integer, as the describe reported it.
	rows, err := db.Query("SELECT CAST(12345.6789 AS NUMERIC(18,4)), " +
		"CAST(90071992547409.93 AS NUMERIC(18,2)) FROM RDB$DATABASE")
	fbsample.Check(err)
	types, err := rows.ColumnTypes()
	fbsample.Check(err)
	_, scale, _ := types[0].DecimalSize()
	fmt.Printf("NUMERIC(18,4) wire format: type=%s, scale=%d, scan type %v\n",
		types[0].DatabaseTypeName(), scale, types[0].ScanType())
	var n4, n2 string
	rows.Next()
	fbsample.Check(rows.Scan(&n4, &n2))
	fbsample.Check(rows.Close())
	raw := strings.Replace(n4, ".", "", 1) // the driver printed raw * 10^scale
	fmt.Printf("value                          : %s\n", n4)
	fmt.Printf("raw integer (digits, no point) : %s  -> %s * 10^%d\n", raw, raw, scale)
	fmt.Printf("NUMERIC(18,2) past 2^53        : %s  <- exact: raw 9007199254740993 never became a float64\n\n", n2)

	// -- 3. INT128: the full range, and one step past it.
	fmt.Println("INT128 max  :", value(db,
		"SELECT CAST(170141183460469231731687303715884105727 AS INT128) FROM RDB$DATABASE"))
	fmt.Println("INT128 max+1:", failure(db,
		"SELECT CAST(170141183460469231731687303715884105727 AS INT128) + 1 FROM RDB$DATABASE"))
	fmt.Println()

	// -- 4. DECFLOAT division by zero: trapped by default, Infinity if untrapped.
	const div = "SELECT CAST(1 AS DECFLOAT(16)) / 0 FROM RDB$DATABASE"
	fmt.Println("1/0 with default traps :", failure(db, div))
	_, err = db.Exec("SET DECFLOAT TRAPS TO") // session-level: this attachment only
	fbsample.Check(err)
	fmt.Println("1/0 with traps cleared :", value(db, div))
	fmt.Println("\ndone.")
}
