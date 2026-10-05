// catalog - the catalog describing itself, from client SQL (Go twin of
// ../../cpp/catalog.cpp; see ../../../catalog-bootstrap.md).
//
// On a freshly created database: the fixed relation ids (RDB$PAGES 0,
// RDB$DATABASE 1, RDB$FIELDS 2, RDB$RELATIONS 6); RDB$PAGES carrying its
// own pointer page, cross-checked against the hdr_PAGES word at byte 28 of
// page 0; RDB$FORMATS empty under a full system catalog (formats as code);
// and user DDL planting the first stored formats.  With the pure-Go
// firebirdsql the fresh-file dance is SQL all the way: fbsample.Recreate
// attaches and runs DROP DATABASE, then the driver's "firebirdsql_createdb"
// variant creates the file on connect (op_create).  The anchor check stays
// as primitive as in every other language: os.File.ReadAt at offset 28 and
// binary.LittleEndian.Uint32.  Run it on the server machine (it reads the
// file the server wrote).
//
// Run:  go run ./catalog [database [local-file]]
package main

import (
	"database/sql"
	"encoding/binary"
	"fmt"
	"os"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// table prints an isql-style listing; headers from rows.Columns() unless given.
func table(db *sql.DB, query string, names ...string) {
	rs, err := db.Query(query)
	fbsample.Check(err)
	defer rs.Close()
	cols, _ := rs.Columns()
	if len(names) == 0 {
		names = cols
	}
	widths := make([]int, len(names))
	for i, n := range names {
		widths[i] = len(n)
	}
	var cells [][]string
	vals := make([]any, len(cols))
	ptrs := make([]any, len(cols))
	for i := range vals {
		ptrs[i] = &vals[i]
	}
	for rs.Next() {
		fbsample.Check(rs.Scan(ptrs...))
		row := make([]string, len(cols))
		for i, v := range vals {
			row[i] = strings.TrimSpace(fbsample.Text(v))
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
	database := fbsample.DBPath("catalog")
	localFile := database
	if len(os.Args) > 2 {
		localFile = os.Args[2]
	}

	// A truly fresh database each run: drop it if it exists, recreate.
	db, err := fbsample.Recreate(database)
	fbsample.Check(err)
	defer db.Close()

	fmt.Println("-- 1. fixed relation ids (relations.h declaration order) --")
	table(db, `select rdb$relation_id, trim(rdb$relation_name) from rdb$relations
	            where rdb$relation_id in (0, 1, 2, 6) order by 1`, "ID", "NAME")

	fmt.Println("\n-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --")
	table(db, `select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type
	             from rdb$pages where rdb$relation_id in (0, 6)
	            order by rdb$relation_id, rdb$page_type, rdb$page_number`)

	// The anchor that cuts the recursion: hdr_PAGES at byte 28 of page 0.
	if f, err := os.Open(localFile); err != nil {
		fmt.Printf("\n(cannot read %s for the hdr_PAGES check: %v)\n", localFile, err)
	} else {
		var word [4]byte
		_, err := f.ReadAt(word[:], 28)
		f.Close()
		fbsample.Check(err)
		fmt.Printf("\nhdr_PAGES (page 0, offset 28) = %d  <- matches the (relation 0, type 4) row above\n",
			binary.LittleEndian.Uint32(word[:]))
	}

	fmt.Println("\n-- 3. formats as code: zero stored formats, yet a full catalog --")
	table(db, `select (select count(*) from rdb$formats),
	                  (select count(*) from rdb$relations where rdb$system_flag = 1),
	                  (select count(*) from rdb$relation_fields r join rdb$relations rel
	                      on r.rdb$relation_name = rel.rdb$relation_name
	                     and r.rdb$schema_name = rel.rdb$schema_name
	                   where rel.rdb$system_flag = 1)
	             from rdb$database`, "FORMATS_ROWS", "SYS_RELATIONS", "SYS_FIELDS")

	fmt.Println("\n-- 4. user DDL writes formats into the catalog --")
	_, err = db.Exec("create table t1 (a integer)")
	fbsample.Check(err)
	_, err = db.Exec("alter table t1 add b varchar(10)")
	fbsample.Check(err)
	table(db, `select rdb$relation_id, rdb$format, octet_length(rdb$descriptor)
	             from rdb$formats order by rdb$relation_id, rdb$format`,
		"RDB$RELATION_ID", "RDB$FORMAT", "DESCRIPTOR_BYTES")
	relID, err := fbsample.Scalar(db, "select rdb$relation_id from rdb$relations where rdb$relation_name = 'T1'")
	fbsample.Check(err)
	fmt.Printf("\n(relation id of T1: %s - the first user id; system tables still contribute no rows)\n",
		fbsample.Text(relID))
	fmt.Println("done.")
}
