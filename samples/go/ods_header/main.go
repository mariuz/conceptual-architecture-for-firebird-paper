// ods_header - the header page read straight from the file (Go twin of
// ../../cpp/ods_header.cpp; see ../../../on-disk-structure.md).
//
// Creates (or reuses) a scratch database through the server, commits a few
// transactions so the TIP markers move, then reads the same facts three
// ways: MON$DATABASE (SQL), `gstat -h` run by the server through the
// Services API, and the raw bytes of page 0 decoded at the offsets
// src/jrd/ods.h pins with static_asserts.  It ends with a page-type
// census: byte 0 of every page.
//
// firebirdsql has no public database-info call (it sends isc_info_ods_version
// only as its Ping), so the second view is the driver's ServiceManager:
// GetDbStatsString(WithOnlyHeaderPages()) is isc_action_svc_db_stats with
// sts_hdr_pages - gstat's own header report, produced server-side and
// streamed back over the service connection.  The file half is
// encoding/binary at the ods.h offsets: no driver can abstract the format.
// Run it on the server machine (the server writes the file; we read it -
// the reader needs the firebird group, e.g. `sg firebird -c ...`).
//
// Run:  go run ./ods_header [database [local-file]]
package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
	"github.com/nakagami/firebirdsql"
)

var pageTypes = []string{"undefined", "pag_header", "pag_pages (PIP)", "pag_transactions (TIP)",
	"pag_pointer", "pag_data", "pag_root", "pag_index (b-tree)", "pag_blob",
	"pag_ids (generators)", "pag_scns"}

func pageTypeName(t int) string {
	if t >= 0 && t < len(pageTypes) {
		return pageTypes[t]
	}
	return "???"
}

func main() {
	path := fbsample.DBPath("ods")
	local := path
	if len(os.Args) > 2 {
		local = os.Args[2]
	}

	// 1. The server's view, while attached.
	db, err := fbsample.Create(path)
	fbsample.Check(err)
	for i := 0; i < 3; i++ {
		tx, err := db.Begin()
		fbsample.Check(err)
		_, err = fbsample.Scalar(tx, "select 1 from rdb$database")
		fbsample.Check(err)
		fbsample.Check(tx.Commit())
	}
	fmt.Println("-- server's view (MON$DATABASE) --")
	var ps, major, minor, oit, oat, ost, next int64
	fbsample.Check(db.QueryRow("select mon$page_size, mon$ods_major, mon$ods_minor,"+
		" mon$oldest_transaction, mon$oldest_active, mon$oldest_snapshot, mon$next_transaction"+
		" from mon$database").Scan(&ps, &major, &minor, &oit, &oat, &ost, &next))
	fmt.Printf("page_size ods_major ods_minor oit oat ost next = %d %d %d %d %d %d %d\n",
		ps, major, minor, oit, oat, ost, next)
	db.Close()

	// 2. The server reading its own header page: gstat -h via Services.
	sm, err := firebirdsql.NewServiceManager(fbsample.ServiceAddr(), fbsample.User,
		fbsample.Password, fbsample.ServiceOptions())
	fbsample.Check(err)
	stats, err := sm.GetDbStatsString(path,
		firebirdsql.NewStatisticsOptions(firebirdsql.WithOnlyHeaderPages()))
	sm.Close()
	fbsample.Check(err)
	fmt.Println("\n-- the same through the Services API (gstat -h, server-side) --")
	for _, line := range strings.Split(stats, "\n") {
		l := strings.TrimSpace(line)
		for _, key := range []string{"Page size", "ODS version", "Oldest transaction",
			"Oldest active", "Oldest snapshot", "Next transaction", "Database GUID"} {
			if strings.HasPrefix(l, key) {
				fmt.Println("  " + strings.Join(strings.Fields(l), " "))
			}
		}
	}

	// 3. Now the same facts straight from the bytes on disk.
	f, err := os.Open(local)
	fbsample.Check(err)
	defer f.Close()
	h := make([]byte, 152) // sizeof(Ods::header_page)
	_, err = io.ReadFull(f, h)
	fbsample.Check(err)
	le := binary.LittleEndian

	fmt.Printf("\n-- header page, parsed from %s (offsets per ods.h) --\n", local)
	fmt.Printf("pag_type      @0   = %d (%s)\n", h[0], pageTypeName(int(h[0])))
	fmt.Printf("pag_flags     @1   = %d\n", h[1])
	pageSize := int64(le.Uint16(h[16:]))
	ods := le.Uint16(h[18:])
	fmt.Printf("hdr_page_size @16  = %d\n", pageSize)
	set := "clear"
	if ods&0x8000 != 0 {
		set = "set"
	}
	fmt.Printf("hdr_ods_version @18 = 0x%04x -> ODS %d (FIREBIRD flag 0x8000 %s), minor @20 = %d\n",
		ods, ods&0x7fff, set, le.Uint16(h[20:]))
	flags := le.Uint16(h[22:])
	names := []string{}
	for _, fl := range []struct {
		bit  uint16
		name string
	}{{0x2, "force_write"}, {0x8, "no_reserve"}, {0x10, "SQL_dialect_3"}} {
		if flags&fl.bit != 0 {
			names = append(names, fl.name)
		}
	}
	fmt.Printf("hdr_flags     @22  = 0x%02x (%s)\n", flags, strings.Join(names, " "))
	fmt.Printf("hdr_PAGES     @28  = %d   <- pointer page of RDB$PAGES (catalog bootstrap anchor)\n",
		le.Uint32(h[28:]))
	fmt.Printf("hdr_next_transaction   @40 = %d\n", le.Uint64(h[40:]))
	fmt.Printf("hdr_oldest_transaction @48 = %d (OIT)\n", le.Uint64(h[48:]))
	fmt.Printf("hdr_oldest_active      @56 = %d (OAT)\n", le.Uint64(h[56:]))
	fmt.Printf("hdr_oldest_snapshot    @64 = %d (OST)\n", le.Uint64(h[64:]))
	g := h[84:100] // hdr_guid: Win32 GUID layout
	fmt.Printf("hdr_guid      @84  = {%08X-%04X-%04X-%X-%X}\n",
		le.Uint32(g), le.Uint16(g[4:]), le.Uint16(g[6:]), g[8:10], g[10:16])

	// 4. Page-type census: byte 0 of every page.
	counts := make([]int, len(pageTypes))
	var pages int64
	b := make([]byte, 1)
	for ; ; pages++ {
		if _, err := f.ReadAt(b, pages*pageSize); err != nil {
			break
		}
		t := int(b[0])
		if t >= len(pageTypes) {
			t = 0
		}
		counts[t]++
	}
	fmt.Printf("\n-- page-type census: %d pages of %d bytes --\n", pages, pageSize)
	for t, c := range counts {
		if c > 0 {
			fmt.Printf("  type %2d  %-22s %5d\n", t, pageTypeName(t), c)
		}
	}
	fmt.Println("done.")
}
