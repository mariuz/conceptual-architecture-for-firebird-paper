// embedded_demo - what a pure-wire Go client can and cannot embed (Go twin
// of ../../cpp/embedded_demo.cpp; see ../../../embedded-architecture-comparison.md).
//
// The C++ twin watches /proc/self/maps as the Y-valve loads libEngine14.so
// into the process, runs DDL/DML with no server, and times an embedded
// attach against a remote one.  firebirdsql has no embedded mode: there is
// no native code to load, so this sample runs the same three steps and
// reports what actually happens - the process map never gains a Firebird
// library (not even libfbclient), the "local" scratch database is created
// and served by the server process (NETWORK_PROTOCOL TCPv4, MON$SERVER_PID
// is not ours), and the timing compares the only thing a wire client can
// vary: the wire itself - an attach to the local-path database and to the
// employee alias, both over TCP, then employee again while another
// attachment holds it open - which shows that most of the remote-vs-local
// gap is the server opening a cold database, not the socket.  A plaintext
// (wire_crypt=false) attach is refused by the server's default
// WireCrypt = Required.
//
// Run:  go run ./embedded_demo [local-path] [remote-db]
package main

import (
	"database/sql"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func mapped(frag string) string {
	b, err := os.ReadFile("/proc/self/maps")
	if err == nil && strings.Contains(string(b), frag) {
		return "yes"
	}
	return "no"
}

func maps(when string) {
	fmt.Printf("%-14s libfbclient mapped=%s, libEngine14 mapped=%s\n", when, mapped("libfbclient"), mapped("libEngine14"))
}

func attachMs(path string, params ...string) float64 {
	t0 := time.Now()
	db, err := fbsample.Attach(path, params...) // Ping = attach on the wire
	fbsample.Check(err)
	db.Close() // op_detach
	return float64(time.Since(t0).Microseconds()) / 1000
}

func main() {
	local, remote := fbsample.Scratch+"/embedded_demo_go.fdb", "employee"
	if len(os.Args) > 1 {
		local = os.Args[1]
	}
	if len(os.Args) > 2 {
		remote = os.Args[2]
	}

	// --- 1. the process map: nothing to load, before or after ------------
	maps("before attach:")
	db, err := fbsample.Create(local)
	fbsample.Check(err)
	defer db.Close()
	maps("after  attach:")
	fmt.Println()

	// --- 2. real work - done by the server process -----------------------
	db.Exec("drop table gadgets") // first run: nothing to drop
	_, err = db.Exec("create table gadgets (id int primary key, name varchar(20))")
	fbsample.Check(err)
	for i, n := range []string{"sprocket", "flange", "grommet"} {
		_, err = db.Exec("insert into gadgets values (?, ?)", i+1, n)
		fbsample.Check(err)
	}
	var rows int
	var maxName string
	var proto sql.NullString
	var serverPid int
	fbsample.Check(db.QueryRow(`select count(*), max(name),
	    rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), a.mon$server_pid
	  from gadgets, mon$attachments a
	  where a.mon$attachment_id = current_connection
	  group by 3, 4`).Scan(&rows, &maxName, &proto, &serverPid))
	fmt.Printf("rows=%d  max(name)=%s  NETWORK_PROTOCOL=%s\n", rows, maxName, proto.String)
	fmt.Printf("engine pid=%d, my pid=%d - the engine is the server's, the path was resolved there\n\n",
		serverPid, os.Getpid())

	// --- 3. attach cost: always a socket + SRP; only the wire varies -----
	const runs = 5
	cases := []struct {
		label, path string
		params      []string
	}{
		{"local path", local, nil},
		{"remote", remote, nil},
	}
	for _, c := range cases {
		attachMs(c.path, c.params...) // warm-up
	}
	fmt.Printf("attach+detach avg over %d runs (every one over TCP, no embedded row):\n", runs)
	for _, c := range cases {
		total := 0.0
		for i := 0; i < runs; i++ {
			total += attachMs(c.path, c.params...)
		}
		fmt.Printf("    %-26s %-34s %7.2f ms\n", c.label, c.path, total/runs)
	}
	// The gap above is not embedded-vs-remote: both are TCP.  The local-path
	// database is held open by this program's first attachment; employee
	// is opened cold by every attach.  Hold employee open too and re-time.
	keeper, err := fbsample.Attach(remote)
	fbsample.Check(err)
	total := 0.0
	for i := 0; i < runs; i++ {
		total += attachMs(remote)
	}
	keeper.Close()
	fmt.Printf("    %-26s %-34s %7.2f ms\n", "remote, db kept open", remote, total/runs)

	// The one wire knob this server will not turn: WireCrypt defaults to
	// Required on the server side, so a plaintext attach is refused.
	if _, err := fbsample.Attach(remote, "charset=UTF8", "wire_crypt=false"); err != nil {
		fmt.Printf("    %-26s refused: %s\n", "remote, wire_crypt=false", fbsample.ErrText(err))
	}
	maps("at exit:")
	fmt.Println("done.")
}
