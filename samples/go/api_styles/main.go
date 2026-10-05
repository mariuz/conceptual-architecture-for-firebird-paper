// api_styles - one query, three levels of a Path B driver (Go twin of
// ../../cpp/api_styles.cpp; see ../../../client-apis-and-drivers.md).
//
// The C++ twin runs one SELECT through both C APIs of libfbclient (ISC and
// OO).  firebirdsql loads no client library at all: it is a Path B driver,
// an independent implementation of the wire protocol in Go.  So its
// "levels" are Go's own: the portable database/sql surface; the driver
// beneath it, reached with sql.Conn.Raw - database/sql/driver's
// Prepare / Query / Next(dest []driver.Value) row protocol plus the
// driver's extras (ProtocolVersion, WireCipher, ExecImmediate); and the
// Services API, which the driver also speaks itself.  The error model is
// the status vector as a Go value: *firebirdsql.FbError carries the GDS
// codes, SQLCODE and SQLSTATE that libfbclient clients read from
// ISC_STATUS slots.
//
// Run:  go run ./api_styles [database]
package main

import (
	"context"
	"database/sql/driver"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/nakagami/firebirdsql"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const query = "select rdb$get_context('SYSTEM', 'ENGINE_VERSION') from rdb$database"

// The extras firebirdsql's driver connection offers beyond driver.Conn.
type fbConn interface {
	driver.Conn
	ProtocolVersion() int
	WireCipher() string
	ExecImmediate(ctx context.Context, sql string) error
}

func main() {
	database := "employee"
	if len(os.Args) > 1 {
		database = os.Args[1]
	}
	db, err := fbsample.Attach(database, "charset=NONE")
	fbsample.Check(err)
	defer db.Close()

	// 1. database/sql: portable, pooled, typed Scan.
	var version string
	fbsample.Check(db.QueryRow(query).Scan(&version))
	fmt.Println("[database/sql ] engine version =", version)

	// 2. One level down: the driver connection itself.
	conn, err := db.Conn(context.Background())
	fbsample.Check(err)
	fbsample.Check(conn.Raw(func(dc any) error {
		fc := dc.(fbConn)
		stmt, err := fc.Prepare(query)
		if err != nil {
			return err
		}
		defer stmt.Close()
		rows, err := stmt.Query(nil) //nolint:staticcheck // the raw driver protocol
		if err != nil {
			return err
		}
		defer rows.Close()
		dest := make([]driver.Value, len(rows.Columns()))
		for {
			if err := rows.Next(dest); err == io.EOF {
				break
			} else if err != nil {
				return err
			}
			// charset NONE text arrives raw: database/sql's Scan made it a string.
			fmt.Printf("[driver.Conn  ] engine version = %s   (driver.Value %T; protocol %d, %s, no libfbclient)\n",
				dest[0], dest[0], fc.ProtocolVersion(), fc.WireCipher())
		}
		// op_execute_immediate: no prepare, no statement handle.
		return fc.ExecImmediate(context.Background(), "set decfloat round half_even")
	}))
	conn.Close()
	fmt.Println("[driver.Conn  ] ExecImmediate(\"set decfloat round half_even\") ok")

	// 3. The Services API, spoken by the same driver.
	sm, err := firebirdsql.NewServiceManager(fbsample.ServiceAddr(), fbsample.User, fbsample.Password,
		fbsample.ServiceOptions())
	fbsample.Check(err)
	sv, err := sm.GetServerVersionString()
	fbsample.Check(err)
	sm.Close()
	fmt.Println("[service_mgr  ] server version =", sv)

	// The error model: the status vector, as a Go struct.
	_, err = fbsample.Attach("/nonexistent/x.fdb")
	if fe, ok := fbsample.FbError(err); ok {
		fmt.Println("[error model  ] attach /nonexistent/x.fdb ->")
		fmt.Println("    " + strings.ReplaceAll(fbsample.ErrText(err), "\n", "\n    "))
		fmt.Printf("    GDSCodes %v  SQLCode %d  SQLState %s\n", fe.GDSCodes, fe.SQLCode, fe.SQLState)
	} else {
		fmt.Println("unexpected:", err)
	}
	fmt.Println("one wire protocol, no client library, three Go levels. done.")
}
