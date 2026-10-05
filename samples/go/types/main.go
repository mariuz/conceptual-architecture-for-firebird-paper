// types - Firebird's headline types round-tripped from Go (Go twin of
// ../../cpp/types.cpp; see ../../../sql-dialect-and-types.md).
//
// One table: BOOLEAN, INT128 at its maximum, DECFLOAT(34) holding an exact
// 0.1, TIMESTAMP WITH TIME ZONE with a named zone, and a CHECK-constrained
// domain (an invalid insert shows the CHECK travelling with the type).
// Shown: the wire types from the output metadata, then every value fetched.
//
// database/sql's ColumnTypes() is the public metadata surface here: the
// driver's DatabaseTypeName() names the wire type (its xsqlvar table) and
// ScanType() says what Go type it decodes to.  Go has no built-in 128-bit
// integer or decimal type and the driver adds no dependency for one, so
// INT128 and DECFLOAT(34) arrive as exact decimal STRINGS - the driver
// decodes the wire bytes itself (big.Int; its own IEEE 754 decimal128
// decoder), and the caller picks the numeric type (math/big here).  Nothing
// is rounded through float64 on the way.
//
// Run:  go run ./types [database]
package main

import (
	"fmt"
	"math/big"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func main() {
	db, err := fbsample.Create(fbsample.DBPath("types"))
	fbsample.Check(err)
	defer db.Close()

	// Idempotent cleanup, each drop committed on its own (DFW).
	db.Exec("DROP TABLE showcase")
	db.Exec("DROP DOMAIN d_email")
	_, err = db.Exec("CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')")
	fbsample.Check(err)
	_, err = db.Exec("CREATE TABLE showcase (" +
		"  flag  BOOLEAN," +
		"  big   INT128," +
		"  money DECFLOAT(34)," +
		"  born  TIMESTAMP WITH TIME ZONE," +
		"  mail  d_email)")
	fbsample.Check(err)
	_, err = db.Exec("INSERT INTO showcase VALUES (" +
		"  TRUE," +
		"  170141183460469231731687303715884105727," + // INT128 max
		"  0.1," + // exact in DECFLOAT
		"  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest'," + // named zone
		"  'user@example.com')")
	fbsample.Check(err)

	// The domain's CHECK travels with the type: a mail without '@' dies.
	_, err = db.Exec("INSERT INTO showcase (mail) VALUES ('not-an-address')")
	if fe, ok := fbsample.FbError(err); ok {
		fmt.Printf("domain CHECK rejected 'not-an-address':\n    sqlcode %d, gds %d: %.110s\n\n",
			fe.SQLCode, fe.GDSCodes[0], fbsample.ErrText(err))
	} else {
		fmt.Println("BUG: domain CHECK did not fire:", err)
	}

	rows, err := db.Query("SELECT * FROM showcase")
	fbsample.Check(err)
	cols, err := rows.ColumnTypes()
	fbsample.Check(err)
	fmt.Println("column  wire type (DatabaseTypeName)  go type (ScanType)")
	fmt.Println("------  ---------------------------  ------------------")
	for _, c := range cols {
		fmt.Printf("%-7s %-28s %s\n", c.Name(), c.DatabaseTypeName(), c.ScanType())
	}

	var flag bool
	var bigText, money, mail string
	var born time.Time
	fbsample.Check(rows.Err())
	rows.Next()
	fbsample.Check(rows.Scan(&flag, &bigText, &money, &born, &mail))
	rows.Close()

	max128 := new(big.Int).Sub(new(big.Int).Lsh(big.NewInt(1), 127), big.NewInt(1))
	b, _ := new(big.Int).SetString(bigText, 10)
	tenth, _ := new(big.Rat).SetString(money)
	fmt.Println("\ntyped round-trip:")
	fmt.Println("  FLAG ", flag)
	fmt.Printf("  BIG   %s  == 2^127 - 1 (big.Int) ? %v\n", bigText, b.Cmp(max128) == 0)
	fmt.Printf("  MONEY %q  == 1/10 exactly (big.Rat) ? %v  (and float64 0.1? %v)\n",
		money, tenth.Cmp(big.NewRat(1, 10)) == 0, tenth.Cmp(new(big.Rat).SetFloat64(0.1)) == 0)
	fmt.Printf("  BORN  %s  Location=%s\n", born.Format(time.RFC3339), born.Location())
	fmt.Printf("  MAIL  %q\n", mail)
}
