// architecture_comparison - one engine, but only one way in from a pure
// wire client (Go twin of ../../cpp/architecture_comparison.cpp; see
// ../../../architecture-comparison.md).
//
// The C++ twin attaches twice through libfbclient: inet://localhost/employee
// goes to the Y-valve's Remote provider, a bare local path loads the Engine
// provider INTO the process.  firebirdsql has no Y-valve and no providers:
// it is a reimplementation of the wire protocol, so every attachment -
// including the "local path" one - is a TCP conversation with a server
// process that resolves the path with ITS rights.  The sample asks the
// engine the same three questions for both attachments (ENGINE_VERSION,
// NETWORK_PROTOCOL, MON$SERVER_PID vs our own pid) and shows that the
// second answer never turns into NULL / our pid.  It adds what only the
// wire client can tell about itself: the negotiated protocol version and
// wire cipher, read from the driver connection through sql.Conn.Raw.
//
// Run:  go run ./architecture_comparison [remote-db] [local-path]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"strconv"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// wireInfo is what the driver connection exposes beyond database/sql.
type wireInfo interface {
	ProtocolVersion() int
	WireCipher() string
}

func inspect(label, database string, create bool) {
	var db *sql.DB
	var err error
	if create {
		db, err = fbsample.Create(database)
	} else {
		db, err = fbsample.Attach(database)
	}
	fbsample.Check(err)
	defer db.Close()

	var version, protocol sql.NullString
	var serverPid int64
	fbsample.Check(db.QueryRow(
		`select rdb$get_context('SYSTEM', 'ENGINE_VERSION'),
		        rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'),
		        a.mon$server_pid
		 from mon$attachments a
		 where a.mon$attachment_id = current_connection`).Scan(&version, &protocol, &serverPid))

	conn, err := db.Conn(context.Background())
	fbsample.Check(err)
	defer conn.Close()
	var proto int
	var cipher string
	fbsample.Check(conn.Raw(func(dc any) error {
		w := dc.(wireInfo)
		proto, cipher = w.ProtocolVersion(), w.WireCipher()
		return nil
	}))

	pid := os.Getpid()
	np := "<null>"
	if protocol.Valid {
		np = protocol.String
	}
	verdict := " -- a different process: the server's"
	if strconv.FormatInt(serverPid, 10) == strconv.Itoa(pid) {
		verdict = " -- the engine runs IN this process"
	}
	fmt.Println(label)
	fmt.Println("    connection string :", database)
	fmt.Println("    ENGINE_VERSION    :", version.String)
	fmt.Println("    NETWORK_PROTOCOL  :", np)
	fmt.Printf("    MON$SERVER_PID    : %d   (this process is pid %d%s)\n", serverPid, pid, verdict)
	fmt.Printf("    wire              : protocol %d, cipher %s\n", proto, cipher)
}

func main() {
	remote, local := "employee", fbsample.Scratch+"/arch_go.fdb"
	if len(os.Args) > 1 {
		remote = os.Args[1]
	}
	if len(os.Args) > 2 {
		local = os.Args[2]
	}
	fmt.Println("One pure-Go wire client: no Y-valve, no providers, always a server.")
	fmt.Println()
	inspect("[1] Alias over the wire (client-server):", remote, false)
	fmt.Println()
	inspect("[2] \"Local\" path - still resolved by the server:", local, true)
	fmt.Println()
	fmt.Println("No embedded attachment: the Engine provider lives in libfbclient,")
	fmt.Println("which this driver never loads.")
	fmt.Println("done.")
}
