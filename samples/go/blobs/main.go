// blobs - segmented BLOBs seen from a whole-value driver (Go twin of
// ../../cpp/blobs.cpp; see ../../../blob-handling.md).
//
// firebirdsql has no blob handle API: a blob column scans into a string or
// []byte and a string / []byte parameter is bound like any other value.
// Underneath, the driver picks the path by size - a value shorter than
// 32767 bytes travels as a plain text parameter and the SERVER coerces it
// into the blob column; a longer one becomes op_create_blob2 plus
// op_put_segment calls of 32000 bytes each; reads drain op_get_segment
// into one buffer (or, at protocol 19, take the inline copy the server
// sent with the row).  Segment boundaries and getInfo statistics never
// reach Go code - so this sample goes to the segments from the other
// side, in SQL: an EXECUTE BLOCK walks any stored blob segment by segment
// with RDB$BLOB_UTIL.READ_DATA(h, NULL), which returns one segment per
// call.  The walk exposes the driver's own 32000-byte write segmentation,
// and it shows that the nearest SQL substitute for the C++ twin's three
// putSegment() calls - three BLOB_APPENDs onto RDB$BLOB_UTIL.NEW_BLOB
// (segmented) - is coalesced by the engine into ONE 40-byte segment.
// One trap of the size rule: a short []byte also travels as TEXT in the
// connection charset, so binary data that is not valid UTF8 fails with
// "Malformed string" on a charset=UTF8 attachment (even with a CAST to
// VARBINARY); the sample stores it through a charset=NONE attachment.
//
// Run:  go run ./blobs [database]
package main

import (
	"bytes"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// segmentWalk lists the segments of docs.<col> for one row, server-side.
const segmentWalk = `execute block (id integer = ?) returns (n integer, len integer, seg varchar(40))
as
  declare h integer;
  declare b blob;
  declare d varbinary(32765);
begin
  select %s from docs where id = :id into :b;
  h = rdb$blob_util.open_blob(b);
  n = 0;
  while (true) do
  begin
    d = rdb$blob_util.read_data(h, null);   -- NULL length: one segment
    if (d is null) then leave;
    n = n + 1;
    len = octet_length(d);
    seg = %s;
    suspend;
  end
  execute procedure rdb$blob_util.close_handle(h);
end`

func main() {
	db, err := fbsample.Create(fbsample.DBPath("blobs"))
	fbsample.Check(err)
	defer db.Close()

	_, err = db.Exec(`recreate table docs (id integer primary key,
	  note blob sub_type text character set utf8, data blob sub_type binary)`)
	fbsample.Check(err)

	tx, err := db.Begin()
	fbsample.Check(err)
	defer tx.Rollback()

	segments := func(id int, col string) {
		preview := "cast(left(d, 30) as varchar(40))" // text: first 30 bytes
		if col == "data" {
			preview = "hex_encode(left(d, 12))" // binary: first 12 bytes in hex
		}
		rows, err := fbsample.Rows(tx, fmt.Sprintf(segmentWalk, col, preview), id)
		fbsample.Check(err)
		lens := []string{}
		for _, r := range rows {
			if len(rows) <= 4 {
				fmt.Printf("  segment #%d: %5d bytes  %q\n", r[0], r[1], fbsample.Text(r[2]))
			}
			lens = append(lens, fmt.Sprint(r[1]))
		}
		if len(rows) > 4 {
			fmt.Printf("  %d segments: %s bytes\n", len(rows), strings.Join(lens, ", "))
		}
	}

	// -- 1. Three explicit segments, written in SQL (no blob API in Go).
	_, err = tx.Exec(`execute block as
	  declare b blob sub_type text character set utf8;
	begin
	  b = rdb$blob_util.new_blob(false, true);          -- segmented, temporary
	  b = blob_append(b, 'first segment');
	  b = blob_append(b, 'second, longer segment');
	  b = blob_append(b, 'third');
	  insert into docs (id, note) values (1, :b);
	end`)
	fbsample.Check(err)
	fmt.Println("id 1: 3 BLOB_APPENDs onto RDB$BLOB_UTIL.NEW_BLOB(segmented) - Go has no putSegment")
	var note string
	fbsample.Check(tx.QueryRow("select note from docs where id = 1").Scan(&note))
	fmt.Printf("  Go sees one string: %q (%d bytes)\n", note, len(note))
	fmt.Println("  RDB$BLOB_UTIL.READ_DATA(h, NULL), one segment per call - BLOB_APPEND coalesced them:")
	segments(1, "note")

	// -- 2. A long Go string: the driver creates the blob itself.
	long := strings.Repeat("0123456789", 4000) // 40000 bytes
	_, err = tx.Exec("insert into docs (id, note) values (2, ?)", long)
	fbsample.Check(err)
	fmt.Println("\nid 2: a 40000-byte Go string parameter (>= 32767: op_create_blob2 + op_put_segment)")
	segments(2, "note")

	// -- 3. Binary: []byte in, []byte out.
	bin := make([]byte, 256)
	for i := range bin {
		bin[i] = byte(i)
	}
	// Under 32767 bytes the driver sends a []byte as a TEXT parameter in
	// the connection charset - and these bytes are not valid UTF8.
	_, err = tx.Exec("insert into docs (id, data) values (3, ?)", bin)
	fmt.Printf("\nid 3: 256 binary bytes on the charset=UTF8 attachment -> %s\n", fbsample.ErrText(err))
	raw, err := fbsample.Attach(fbsample.DBPath("blobs"), "charset=NONE")
	fbsample.Check(err)
	_, err = raw.Exec("insert into docs (id, data) values (3, ?)", bin) // autocommit
	fbsample.Check(err)
	raw.Close()
	fmt.Println("      same []byte on a charset=NONE attachment -> stored")
	var back []byte
	fbsample.Check(tx.QueryRow("select data from docs where id = 3").Scan(&back))
	fmt.Printf("      read back as []byte: %d bytes, round-trip intact: %v\n", len(back), bytes.Equal(bin, back))
	segments(3, "data")

	// -- 4. Subtype text vs binary, from the catalog.
	fmt.Println("\n-- column subtypes (RDB$FIELDS) --")
	rows, err := fbsample.Rows(tx, `select trim(rf.rdb$field_name), f.rdb$field_sub_type, trim(cs.rdb$character_set_name)
	  from rdb$relation_fields rf
	  join rdb$fields f on rf.rdb$field_source = f.rdb$field_name
	  left join rdb$character_sets cs on f.rdb$character_set_id = cs.rdb$character_set_id
	  where rf.rdb$relation_name = 'DOCS' and f.rdb$field_type = 261 order by 1`)
	fbsample.Check(err)
	fmt.Println("FIELD SUBTYPE CHARSET")
	fmt.Println("----- ------- -------")
	for _, r := range rows {
		fmt.Printf("%-5s %-7s %s\n", fbsample.Text(r[0]), fbsample.Text(r[1]), fbsample.Text(r[2]))
	}

	// -- 5. BLOB_APPEND in SQL; the result scans into a string, no CAST.
	_, err = tx.Exec(`insert into docs (id, note) values (4,
	  blob_append(cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))`)
	fbsample.Check(err)
	var octets, chars int
	var content string
	fbsample.Check(tx.QueryRow("select octet_length(note), char_length(note), note from docs where id = 4").
		Scan(&octets, &chars, &content))
	fmt.Println("\n-- BLOB_APPEND result --")
	fmt.Printf("id 4: %d octets, %d chars, %q\n", octets, chars, content)

	fbsample.Check(tx.Commit())
	fmt.Println("done.")
}
