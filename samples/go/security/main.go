// security - the security layers observed from client code (Go twin of
// ../../cpp/security.cpp; see ../../../security-architecture.md).
//
// In one run: (1) WHO AM I - this attachment's own MON$ATTACHMENTS row (auth
// plugin and wire-crypt plugin, layers 1 and 2 as the server recorded them),
// set beside what the CLIENT negotiated: firebirdsql implements Srp256 and
// ChaCha64 itself, so the cipher is also readable on the Go side through
// sql.Conn.Raw (WireCipher(), ProtocolVersion()); (2) SEC$USERS, the
// virtual view over the security database; (3) least privilege - a
// temporary user plus a role carrying the MONITOR_ANY_ATTACHMENT system
// privilege, connecting without and with the role; (4) the error chain a
// wrong password produces, as a structured *firebirdsql.FbError.
//
// The role is a DSN parameter (?role=GO_MONITOR), which the driver writes
// as isc_dpb_sql_role_name.  User management is deferred work executed at
// COMMIT, so every CREATE/DROP USER batch runs in its own explicit
// transaction with a full commit (the driver's autocommit mode would
// COMMIT RETAINING instead).  Own names (GO_USER, GO_MONITOR) so the twins
// can run side by side.
//
// Run:  go run ./security [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"net/url"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const (
	tmpUser = "GO_USER"
	tmpPass = "Hands0nPw"
	role    = "GO_MONITOR"
	who     = `select trim(mon$user), mon$auth_method, mon$wire_crypt_plugin,
	                  mon$remote_protocol, trim(coalesce(current_role, 'NONE'))
	             from mon$attachments where mon$attachment_id = current_connection`
	visible = "select count(*) from mon$attachments where mon$system_flag = 0"
)

// commitBatch runs statements in one explicit transaction and commits it.
func commitBatch(db *sql.DB, stmts ...string) error {
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	for _, s := range stmts {
		if _, err := tx.Exec(s); err != nil {
			tx.Rollback()
			return err
		}
	}
	return tx.Commit()
}

// whoAmI prints the attachment's MON$ row and the client-side cipher.
func whoAmI(db *sql.DB, label string) {
	var user, auth, crypt, proto, r string
	fbsample.Check(db.QueryRow(who).Scan(&user, &auth, &crypt, &proto, &r))
	conn, err := db.Conn(context.Background())
	fbsample.Check(err)
	defer conn.Close()
	var client string
	conn.Raw(func(dc any) error {
		if c, ok := dc.(interface {
			WireCipher() string
			ProtocolVersion() int
		}); ok {
			client = fmt.Sprintf("%s/p%d", c.WireCipher(), c.ProtocolVersion())
		}
		return nil
	})
	fmt.Printf("%-22s user=%s auth=%s wirecrypt=%s protocol=%s role=%s  (client: %s)\n",
		label, user, auth, crypt, proto, r, client)
}

// login attaches as the temporary user, optionally with a role.
func login(path, password, withRole string) (*sql.DB, error) {
	// An absolute path yields host//tmp/..., as fbsample.DSN builds it.
	dsn := fmt.Sprintf("%s:%s@%s/%s?charset=UTF8", tmpUser, url.QueryEscape(password),
		fbsample.Host, path)
	if withRole != "" {
		dsn += "&role=" + withRole
	}
	db, err := sql.Open("firebirdsql", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

func count(db *sql.DB) int {
	var n int
	fbsample.Check(db.QueryRow(visible).Scan(&n))
	return n
}

func main() {
	path := fbsample.DBPath("security")
	admin, err := fbsample.Create(path)
	fbsample.Check(err)
	defer admin.Close()

	// 1. Layers 1+2, as recorded for THIS attachment.
	whoAmI(admin, "admin attachment:")

	// 2+3. Temporary user (security database) and privileged role (this
	//      database).  Cleanup first, ignoring errors: DROP USER of a
	//      missing user fails only at COMMIT.
	commitBatch(admin, "drop user "+tmpUser+" using plugin Srp")
	commitBatch(admin, "drop role "+role)
	fbsample.Check(commitBatch(admin,
		"create user "+tmpUser+" password '"+tmpPass+"' using plugin Srp"))
	fbsample.Check(commitBatch(admin,
		"create role "+role+" set system privileges to MONITOR_ANY_ATTACHMENT",
		"grant "+role+" to user "+tmpUser))

	fmt.Println("\nSEC$USERS (the security database, through the virtual view):")
	users, err := fbsample.Rows(admin,
		"select trim(sec$user_name), trim(sec$plugin), sec$admin from sec$users order by 1")
	fbsample.Check(err)
	fmt.Printf("    %-16s %-8s %s\n", "USER", "PLUGIN", "ADMIN")
	for _, u := range users {
		fmt.Printf("    %-16s %-8s %s\n", fbsample.Text(u[0]), fbsample.Text(u[1]), fbsample.Text(u[2]))
	}

	fmt.Printf("\nadmin sees %d user attachments in MON$ATTACHMENTS\n", count(admin))

	plain, err := login(path, tmpPass, "")
	fbsample.Check(err)
	whoAmI(plain, "user, no role:")
	fmt.Printf("  -> sees %d attachment(s): only its own\n", count(plain))
	plain.Close()

	monitor, err := login(path, tmpPass, role)
	fbsample.Check(err)
	whoAmI(monitor, "user + role:")
	fmt.Printf("  -> sees %d attachments: MONITOR_ANY_ATTACHMENT at work\n", count(monitor))
	monitor.Close()

	// 4. The failed login, and its error chain.
	fmt.Println("\nfailed login (wrong password) produces:")
	if bad, err := login(path, "wrong-password", ""); err == nil {
		bad.Close()
		fmt.Println("    (unexpectedly succeeded)")
	} else if fe, ok := fbsample.FbError(err); ok {
		fmt.Printf("    sqlcode %d / sqlstate %s / gds %v\n", fe.SQLCode, fe.SQLState, fe.GDSCodes)
		fmt.Println("    " + strings.ReplaceAll(fbsample.ErrText(err), "\n", "\n    "))
	} else {
		fmt.Println("    " + fbsample.ErrText(err))
	}

	fbsample.Check(commitBatch(admin, "drop user "+tmpUser+" using plugin Srp", "drop role "+role))
	fmt.Println("\ntemporary user and role dropped. done.")
}
