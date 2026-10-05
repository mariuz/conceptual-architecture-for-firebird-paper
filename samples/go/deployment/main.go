// deployment - the engine's own view of its deployment (Go twin of
// ../../cpp/deployment.cpp; see ../../../deployment-and-operations.md).
//
// Three SQL layers, read-only against employee: MON$DATABASE (the physical
// facts of the database), RDB$CONFIG (the EFFECTIVE configuration,
// firebird.conf merged with databases.conf) and the SYSTEM context
// namespace (engine and session).  Then a fourth layer no SQL reaches:
// firebirdsql's ServiceManager asks service_mgr for the install tree -
// server version, architecture, home, security database, lock and message
// directories, attached databases - over the same pure-Go wire stack.
// Session facts are this client's own: firebirdsql negotiates ChaCha64
// like libfbclient (node-firebird gets Arc4), and its RDB$CONFIG_IS_SET
// arrives as a Go bool, rendered client-side.
//
// Run:  go run ./deployment [database]
package main

import (
	"database/sql"
	"fmt"
	"os"
	"strings"

	"github.com/nakagami/firebirdsql"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

func line(db *sql.DB, label, query string) {
	v, err := fbsample.Scalar(db, query)
	fbsample.Check(err)
	fmt.Printf("  %-22s %s\n", label, fbsample.Text(v))
}

func table(db *sql.DB, query string) {
	rs, err := db.Query(query)
	fbsample.Check(err)
	cols, _ := rs.Columns()
	rs.Close()
	rows, err := fbsample.Rows(db, query)
	fbsample.Check(err)
	width := make([]int, len(cols))
	cells := [][]string{cols}
	for _, r := range rows {
		row := make([]string, len(r))
		for i, v := range r {
			row[i] = strings.TrimSpace(fbsample.Text(v))
		}
		cells = append(cells, row)
	}
	for _, r := range cells {
		for i, c := range r {
			width[i] = max(width[i], len(c))
		}
	}
	emit := func(r []string) {
		parts := make([]string, len(r))
		for i, c := range r {
			parts[i] = fmt.Sprintf("%-*s", width[i], c)
		}
		fmt.Println(strings.TrimRight(strings.Join(parts, " "), " "))
	}
	for n, r := range cells {
		emit(r)
		if n == 0 {
			dashes := make([]string, len(r))
			for i := range r {
				dashes[i] = strings.Repeat("-", width[i])
			}
			emit(dashes)
		}
	}
}

func main() {
	database := "employee"
	if len(os.Args) > 1 {
		database = os.Args[1]
	}
	db, err := fbsample.Attach(database, "charset=NONE")
	fbsample.Check(err)
	defer db.Close()

	fmt.Println("== MON$DATABASE: the database as deployed ==")
	line(db, "database file", "SELECT MON$DATABASE_NAME FROM MON$DATABASE")
	line(db, "ODS version", "SELECT MON$ODS_MAJOR || '.' || MON$ODS_MINOR FROM MON$DATABASE")
	line(db, "page size", "SELECT MON$PAGE_SIZE FROM MON$DATABASE")
	line(db, "page buffers", "SELECT MON$PAGE_BUFFERS FROM MON$DATABASE")
	line(db, "sweep interval", "SELECT MON$SWEEP_INTERVAL FROM MON$DATABASE")
	line(db, "forced writes", "SELECT MON$FORCED_WRITES FROM MON$DATABASE")
	line(db, "SQL dialect", "SELECT MON$SQL_DIALECT FROM MON$DATABASE")
	line(db, "crypt state", "SELECT MON$CRYPT_STATE FROM MON$DATABASE")

	n, err := fbsample.Scalar(db, "SELECT COUNT(*) FROM RDB$CONFIG")
	fbsample.Check(err)
	fmt.Printf("\n== RDB$CONFIG: effective configuration (selected of %v settings) ==\n", n)
	table(db, `SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_IS_SET FROM RDB$CONFIG
	  WHERE RDB$CONFIG_NAME IN ('ServerMode', 'DefaultDbCachePages', 'DatabaseAccess',
	    'WireCrypt', 'MaxParallelWorkers', 'SecurityDatabase')
	  ORDER BY RDB$CONFIG_NAME`)

	fmt.Println("\n== settings explicitly set in config files ==")
	table(db, `SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_SOURCE
	  FROM RDB$CONFIG WHERE RDB$CONFIG_IS_SET ORDER BY RDB$CONFIG_ID`)

	fmt.Println("\n== SYSTEM context: this engine, this session ==")
	for _, v := range []string{"ENGINE_VERSION", "DB_NAME", "NETWORK_PROTOCOL", "WIRE_CRYPT_PLUGIN", "CLIENT_ADDRESS"} {
		line(db, v, "SELECT RDB$GET_CONTEXT('SYSTEM', '"+v+"') FROM RDB$DATABASE")
	}

	// The fourth layer: service_mgr, no SQL.
	fmt.Println("\n== service_mgr: the install tree, from firebirdsql's ServiceManager ==")
	sm, err := firebirdsql.NewServiceManager(fbsample.ServiceAddr(), fbsample.User, fbsample.Password,
		fbsample.ServiceOptions())
	fbsample.Check(err)
	defer sm.Close()
	show := func(label string, get func() (string, error)) {
		v, err := get()
		fbsample.Check(err)
		fmt.Printf("  %-22s %s\n", label, v)
	}
	show("server version", sm.GetServerVersionString)
	show("architecture", sm.GetArchitecture)
	show("home directory", sm.GetHomeDir)
	show("security database", sm.GetSecurityDatabasePath)
	show("lock directory", sm.GetLockFileDir)
	show("message directory", sm.GetMsgFileDir)
	info, err := sm.GetSvrDbInfo()
	fbsample.Check(err)
	fmt.Printf("  %-22s %d attachments, %d databases: %s\n", "attached now", info.AttachmentsCount,
		info.DatabaseCount, strings.Join(info.Databases, ", "))
	fmt.Printf("  %-22s %s\n", "service wire cipher", sm.WireCipher())
	fmt.Println("\ndone.")
}
