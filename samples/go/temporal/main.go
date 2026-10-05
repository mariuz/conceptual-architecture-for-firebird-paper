// temporal - WITH TIME ZONE storage seen from the client (Go twin of
// ../../cpp/temporal.cpp; see ../../../temporal-and-time-zones.md).
//
// 1./2. TIMESTAMP '2026-07-18 12:00:00 America/New_York' and the same wall
//
//	time at a bare -05:00 offset, fetched as Go time.Time values.
//
// 3.    AT TIME ZONE across a DST boundary; equality by UTC instant.
// 4.    The session time zone: SET TIME ZONE, and the DPB alternative.
//
// firebirdsql decodes ISC_TIMESTAMP_TZ itself, in Go: the UTC instant plus
// the 2-byte zone id, which it maps to a name through its OWN compiled-in
// copy of Firebird's id table and then hands to time.LoadLocation - so the
// UTC offset is computed by Go's tzdata, not by the engine's ICU.  The
// raw bytes are not exposed, so the struct is reconstructed: days and
// time from the instant, the id from RDB$TIME_ZONES (named zones) or from
// the offset encoding id = 1439 + minutes (bare offsets).  The instructive
// difference: the driver's table has region ids only, so a bare-offset id
// is "unresolvable" and the value falls back to UTC - the instant survives,
// the -05:00 does not.  The session zone at attach time is the DSN's
// ?timezone=, which the driver writes as isc_dpb_session_time_zone.
//
// Run:  go run ./temporal [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const (
	zoneQuery = "select rdb$get_context('SYSTEM', 'SESSION_TIMEZONE') from rdb$database"
	nowQuery  = "select cast(current_timestamp as varchar(50)) from rdb$database"
)

// Firebird's date epoch: ISC_DATE counts days from 1858-11-17.
var epoch = time.Date(1858, 11, 17, 0, 0, 0, 0, time.UTC)

func scalar(db *sql.DB, query string) string {
	v, err := fbsample.Scalar(db, query)
	fbsample.Check(err)
	return strings.TrimSpace(fbsample.Text(v))
}

// zoneID reconstructs the struct's time_zone field from the literal's zone.
func zoneID(db *sql.DB, zone string) int {
	if zone[0] == '+' || zone[0] == '-' {
		var h, m int
		fmt.Sscanf(zone[1:], "%d:%d", &h, &m)
		minutes := h*60 + m
		if zone[0] == '-' {
			minutes = -minutes
		}
		return 1439 + minutes
	}
	var id int
	fbsample.Check(db.QueryRow(
		"select rdb$time_zone_id from rdb$time_zones where rdb$time_zone_name = ?", zone).Scan(&id))
	return id
}

func main() {
	path := fbsample.DBPath("temporal")
	db, err := fbsample.Create(path)
	fbsample.Check(err)
	defer db.Close()

	// -- 1./2. Named zone vs bare offset.
	var named, offset time.Time
	fbsample.Check(db.QueryRow("select timestamp '2026-07-18 12:00:00 America/New_York',"+
		"       timestamp '2026-07-18 12:00:00 -05:00' from rdb$database").Scan(&named, &offset))
	for _, c := range []struct {
		label, zone string
		ts          time.Time
	}{{"named-zone", "America/New_York", named}, {"offset", "-05:00", offset}} {
		utc := c.ts.UTC()
		days := int(utc.Sub(epoch).Hours() / 24)
		frac := utc.Sub(epoch.AddDate(0, 0, days)) / (100 * time.Microsecond)
		fmt.Printf("%s literal:\n", c.label)
		fmt.Printf("  go value     : %s  Location=%s\n", c.ts.Format(time.RFC3339), c.ts.Location())
		fmt.Printf("  as the struct: UTC days=%d time=%d  zone id=%d  (reconstructed)\n",
			days, frac, zoneID(db, c.zone))
	}

	// -- 3. The same wall time across a DST boundary.
	fmt.Println("\nNY 12:00 in UTC, winter:", scalar(db,
		"select cast(timestamp '2026-01-18 12:00:00 America/New_York' at time zone 'Etc/UTC'"+
			" as varchar(50)) from rdb$database"))
	fmt.Println("NY 12:00 in UTC, summer:", scalar(db,
		"select cast(timestamp '2026-07-18 12:00:00 America/New_York' at time zone 'Etc/UTC'"+
			" as varchar(50)) from rdb$database"))
	fmt.Println("10:00 -02:00 = 09:00 -03:00 ?", scalar(db,
		"select iif(time '10:00:00 -02:00' = time '09:00:00 -03:00', 'EQUAL', 'different')"+
			" from rdb$database"))

	// -- 4. The session time zone governs "now".
	fmt.Printf("\nsession zone: %-17s  CURRENT_TIMESTAMP: %s\n", scalar(db, zoneQuery), scalar(db, nowQuery))
	_, err = db.Exec("set time zone 'Asia/Tokyo'")
	fbsample.Check(err)
	fmt.Printf("session zone: %-17s  CURRENT_TIMESTAMP: %s\n", scalar(db, zoneQuery), scalar(db, nowQuery))

	// The same choice made at attach time: ?timezone= puts
	// isc_dpb_session_time_zone in the DPB (and also decodes zoneless
	// DATE/TIME/TIMESTAMP into that zone on the Go side).
	dpb, err := fbsample.Attach(path, "charset=UTF8", "timezone=America/Sao_Paulo")
	fbsample.Check(err)
	fmt.Printf("DSN zone    : %-17s  CURRENT_TIMESTAMP: %s\n", scalar(dpb, zoneQuery), scalar(dpb, nowQuery))
	dpb.Close()
	fmt.Println("\ndone.")
}
