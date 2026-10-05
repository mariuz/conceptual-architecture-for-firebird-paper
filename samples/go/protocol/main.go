// protocol - the negotiated wire session, from both ends (Go twin of
// ../../protocol_client.cpp and ../../python/protocol.py; see
// ../../../firebird-wire-protocol.md).
//
// firebirdsql is an independent implementation of everything in the wire
// document: op_connect with a version list (protocols 10-19), op_cond_accept,
// Srp256 / Srp / Legacy_Auth, op_crypt with ChaCha64 / ChaCha / Arc4, written
// in Go with no libfbclient.  The sample attaches, prints the C++ sample's
// SYSTEM-context answers and the Python twin's MON$ATTACHMENTS row, then the
// client's own view through sql.Conn.Raw (ProtocolVersion, WireCipher).
// Because the handshake is the driver's code, its DSN steers it: a second
// session narrows the cipher list to wire_crypt_plugin=Arc4 and the engine
// records that downgrade; a third offers only the SHA-1 Srp proof
// (auth_plugin_list=Srp) and is refused, because this server's AuthServer
// is Srp256 alone - the downgrade defence lives on the server.
//
// Run:  go run ./protocol [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"os"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

type wireInfo interface {
	ProtocolVersion() int
	WireCipher() string
}

func session(database string, params ...string) {
	db, err := fbsample.Attach(database, params...)
	if err != nil {
		fmt.Println("attach refused :", fbsample.ErrText(err))
		return
	}
	defer db.Close()

	ctx := func(name string) string {
		var v sql.NullString
		fbsample.Check(db.QueryRow("select rdb$get_context('SYSTEM', '" + name + "') from rdb$database").Scan(&v))
		if !v.Valid {
			return "(none)"
		}
		return v.String
	}
	fmt.Println("engine version :", ctx("ENGINE_VERSION"))
	fmt.Println("protocol       :", ctx("NETWORK_PROTOCOL"))
	fmt.Println("wire crypt     :", ctx("WIRE_CRYPT_PLUGIN"))
	var user string
	fbsample.Check(db.QueryRow("select trim(current_user) from rdb$database").Scan(&user))
	fmt.Println("authenticated  :", user)

	var auth, remote, crypt, client sql.NullString
	fbsample.Check(db.QueryRow(`SELECT MON$AUTH_METHOD, MON$REMOTE_VERSION, MON$WIRE_CRYPT_PLUGIN,
	       MON$CLIENT_VERSION FROM MON$ATTACHMENTS
	 WHERE MON$ATTACHMENT_ID = CURRENT_CONNECTION`).Scan(&auth, &remote, &crypt, &client))
	fmt.Println("MON$ATTACHMENTS, as the server recorded the handshake:")
	fmt.Printf("   auth method    : %s\n", auth.String)
	fmt.Printf("   wire protocol  : %s\n", remote.String)
	fmt.Printf("   wire crypt     : %s\n", crypt.String)
	if client.Valid {
		fmt.Printf("   client version : %s\n", client.String)
	} else {
		fmt.Printf("   client version : <null>   (no isc_dpb_client_version: not libfbclient)\n")
	}

	conn, err := db.Conn(context.Background())
	fbsample.Check(err)
	defer conn.Close()
	fbsample.Check(conn.Raw(func(dc any) error {
		w := dc.(wireInfo)
		fmt.Printf("the client side, from the driver: protocol %d, cipher %s\n", w.ProtocolVersion(), w.WireCipher())
		return nil
	}))
}

func main() {
	database := "employee"
	if len(os.Args) > 1 {
		database = os.Args[1]
	}
	fmt.Println("== default DSN: firebirdsql's preferences ==")
	fmt.Println("attached to", database)
	session(database, "charset=NONE")

	fmt.Println("\n== wire_crypt_plugin=Arc4: the client narrows the cipher list ==")
	session(database, "charset=NONE", "wire_crypt_plugin=Arc4")

	fmt.Println("\n== auth_plugin_list=Srp: the client offers only the SHA-1 proof ==")
	session(database, "charset=NONE", "auth_plugin_name=Srp", "auth_plugin_list=Srp")
	fmt.Println("detached. bye")
}
