// intl - charset, collation and transliteration (Go twin of
// ../../cpp/intl.cpp; see ../../../internationalization.md).
//
//  1. Collation decides equality: 'Café', 'CAFE', 'cafe' match 'cafe' three
//     times under UNICODE_CI_AI and once under UCS_BASIC; UPPER() folds
//     accented letters; the two collations sort differently.
//  2. Per-column charsets: a WIN1252 column beside two UTF8 ones.
//  3. Transliteration is chosen by the CONNECTION charset (lc_ctype): the
//     same stored WIN1252 'Café' read over differently configured
//     attachments.
//
// firebirdsql's DSN "charset=" is the lc_ctype AND the driver's codec, and
// its NONE behaviour is the one no other twin shows: with no codec to
// apply, a text column comes back as []byte - the raw stored bytes, handed
// over untouched, neither decoded as latin1 (node-firebird) nor rejected as
// invalid UTF-8 (rsfbclient, firebird-driver).  The sample adds a third
// connection, charset=WIN1252, where the server transliterates to WIN1252
// and the driver decodes client-side through golang.org/x/text into a Go
// (UTF-8) string - transliteration split across the wire.
//
// Run:  go run ./intl [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func hexdump(label string, v any) {
	var b []byte
	kind := fmt.Sprintf("%T", v)
	switch x := v.(type) {
	case []byte:
		b = x
	case string:
		b = []byte(x)
	}
	hex := []string{}
	for _, c := range b {
		hex = append(hex, fmt.Sprintf("%02X", c))
	}
	fmt.Printf("  %-17s %-7s len=%2d  %-15s %q\n", label, kind, len(b), strings.Join(hex, " "), b)
}

func names(db *sql.DB, query string) string {
	rows, err := fbsample.Rows(db, query)
	fbsample.Check(err)
	out := []string{}
	for _, r := range rows {
		out = append(out, fbsample.Text(r[0]))
	}
	return strings.Join(out, "  ")
}

// The row is picked with an ASCII-only predicate, so the SQL text means the
// same thing under every connection charset.
const pick = "SELECT name_win FROM t WHERE name_bin STARTING WITH 'Caf'"

func fetchOver(path, charset string) any {
	db, err := fbsample.Attach(path, "charset="+charset)
	fbsample.Check(err)
	defer db.Close()
	v, err := fbsample.Scalar(db, pick)
	fbsample.Check(err)
	return v
}

func main() {
	path := fbsample.DBPath("intl")
	db, err := fbsample.Create(path) // charset=UTF8 (fbsample's default)
	fbsample.Check(err)
	defer db.Close()

	db.Exec("DROP TABLE t")
	_, err = db.Exec("CREATE TABLE t (" +
		"  name_ci_ai VARCHAR(30) CHARACTER SET UTF8 COLLATE UNICODE_CI_AI," +
		"  name_bin   VARCHAR(30) CHARACTER SET UTF8 COLLATE UCS_BASIC," +
		"  name_win   VARCHAR(30) CHARACTER SET WIN1252)")
	fbsample.Check(err)
	for _, v := range []string{"Café", "CAFE", "cafe"} {
		_, err = db.Exec("INSERT INTO t VALUES (?, ?, ?)", v, v, v)
		fbsample.Check(err)
	}

	// -- 1. The collation, not the data, decides what "equal" means.
	n, err := fbsample.Scalar(db, "SELECT COUNT(*) FROM t WHERE name_ci_ai = 'cafe'")
	fbsample.Check(err)
	fmt.Println("rows matching 'cafe' with UNICODE_CI_AI :", n)
	n, err = fbsample.Scalar(db, "SELECT COUNT(*) FROM t WHERE name_bin = 'cafe'")
	fbsample.Check(err)
	fmt.Println("rows matching 'cafe' with UCS_BASIC     :", n)
	up, err := fbsample.Scalar(db, "SELECT UPPER('café èñ ß') FROM RDB$DATABASE")
	fbsample.Check(err)
	fmt.Println("UPPER('café èñ ß')                      :", fbsample.Text(up))
	fmt.Println()
	fmt.Println("ORDER BY name_ci_ai:", names(db, "SELECT name_ci_ai FROM t ORDER BY name_ci_ai"))
	fmt.Println("ORDER BY name_bin  :", names(db, "SELECT name_bin FROM t ORDER BY name_bin"),
		"   (binary: uppercase codepoints first)")
	fmt.Println()

	// -- 2./3. Same stored WIN1252 'Café', three connection charsets.
	fmt.Println("SELECT name_win ... 'Café' - same row, three connections (Go type, bytes):")
	hexdump("charset=UTF8:", fetchOver(path, "UTF8"))
	hexdump("charset=NONE:", fetchOver(path, "NONE"))
	hexdump("charset=WIN1252:", fetchOver(path, "WIN1252"))
	fmt.Println("  -> the column stores E9 (WIN1252).  UTF8: the server transliterated to C3 A9.\n" +
		"     NONE: the raw stored byte, as []byte - the driver has no codec to apply.\n" +
		"     WIN1252: E9 crossed the wire and x/text decoded it into a Go string.")
	fmt.Println("\ndone.")
}
