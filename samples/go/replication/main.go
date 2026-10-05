// replication - the client-visible half of Firebird replication: the
// publication, walked through its states with plain DDL and read back from
// the system tables (Go twin of ../../cpp/replication.cpp; see
// ../../../replication-architecture.md).
//
//	ALTER DATABASE ENABLE PUBLICATION              -> RDB$PUBLICATIONS
//	ALTER DATABASE INCLUDE TABLE ... / INCLUDE ALL -> RDB$PUBLICATION_TABLES
//
// The journal/segment transport behind it needs server-side replication.conf
// and stays as text in the document.  What this twin adds is the OTHER end
// of the relationship, reachable because firebirdsql ships a Services
// MaintenanceManager: SetReplicaMode sends isc_spb_prp_replica_mode (the
// gfix -replica switch) over the service manager, so the sample turns its
// scratch database into a read-only replica, shows MON$REPLICA_MODE = 1 and
// a user write being refused, then turns it back into a primary.
//
// Run:  go run ./replication [database]
package main

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
	"github.com/nakagami/firebirdsql"
)

func exec(db *sql.DB, q string) {
	_, err := db.Exec(q)
	fbsample.Check(err)
}

func pubState(db *sql.DB, when string) {
	fmt.Println("--", when)
	pubs, err := fbsample.Rows(db, "SELECT TRIM(RDB$PUBLICATION_NAME), RDB$ACTIVE_FLAG, "+
		"RDB$AUTO_ENABLE FROM RDB$PUBLICATIONS")
	fbsample.Check(err)
	tables, err := fbsample.Rows(db, "SELECT TRIM(RDB$TABLE_SCHEMA_NAME) || '.' || "+
		"TRIM(RDB$TABLE_NAME) FROM RDB$PUBLICATION_TABLES ORDER BY RDB$TABLE_NAME")
	fbsample.Check(err)
	names := []string{}
	for _, t := range tables {
		names = append(names, fbsample.Text(t[0]))
	}
	if len(names) == 0 {
		names = []string{"(none)"}
	}
	for _, p := range pubs {
		fmt.Printf("%-13s ACTIVE_FLAG %v   AUTO_ENABLE %v    published: %s\n",
			fbsample.Text(p[0]), p[1], p[2], strings.Join(names, ", "))
	}
}

func replicaMode(db *sql.DB) any {
	v, err := fbsample.Scalar(db, "SELECT MON$REPLICA_MODE FROM MON$DATABASE")
	fbsample.Check(err)
	return v
}

func setReplica(path string, mode firebirdsql.ReplicaMode) {
	mm, err := firebirdsql.NewMaintenanceManager(fbsample.ServiceAddr(),
		fbsample.User, fbsample.Password, fbsample.ServiceOptions())
	fbsample.Check(err)
	fbsample.Check(mm.SetReplicaMode(path, mode))
}

func main() {
	path := fbsample.DBPath("replication")
	db, err := fbsample.Create(path)
	fbsample.Check(err)

	// Idempotent reset: back to a clean, unpublished state.  Each failure
	// dooms only its own statement, so the errors are simply ignored.
	for _, q := range []string{
		"ALTER DATABASE EXCLUDE ALL FROM PUBLICATION",
		"ALTER DATABASE DISABLE PUBLICATION",
		"DROP TABLE REPL_ORDERS",
		"DROP TABLE REPL_SCRATCH",
	} {
		db.Exec(q)
	}
	exec(db, "CREATE TABLE REPL_ORDERS (ID INT NOT NULL PRIMARY KEY, ITEM VARCHAR(30))")
	exec(db, "CREATE TABLE REPL_SCRATCH (N INT)") // note: no key

	pubState(db, "initial state (publication exists but is inactive)")
	exec(db, "ALTER DATABASE ENABLE PUBLICATION")
	pubState(db, "after ENABLE PUBLICATION")
	exec(db, "ALTER DATABASE INCLUDE TABLE REPL_ORDERS TO PUBLICATION")
	pubState(db, "after INCLUDE TABLE REPL_ORDERS")
	exec(db, "ALTER DATABASE INCLUDE ALL TO PUBLICATION")
	pubState(db, "after INCLUDE ALL (auto-enable: future tables join automatically)")
	fmt.Printf("\nMON$REPLICA_MODE = %v  (0 = not a replica: this side publishes)\n", replicaMode(db))
	db.Close()

	// The other end: make the scratch database a read-only replica through
	// the Services API, look at it, and make it a primary again.
	setReplica(path, firebirdsql.ReplicaModeReadOnly)
	fmt.Println("\n-- Services: MaintenanceManager.SetReplicaMode(ReplicaModeReadOnly)")
	rep, err := fbsample.Attach(path)
	fbsample.Check(err)
	fmt.Printf("MON$REPLICA_MODE = %v  (1 = read-only replica)\n", replicaMode(rep))
	if _, err := rep.Exec("INSERT INTO REPL_ORDERS VALUES (1, 'widget')"); err != nil {
		fmt.Println("user INSERT on the replica:", strings.ReplaceAll(fbsample.ErrText(err), "\n", " / "))
	} else {
		fmt.Println("BUG: user INSERT on a read-only replica succeeded")
	}
	rep.Close()

	setReplica(path, firebirdsql.ReplicaModeNone)
	fmt.Println("-- Services: MaintenanceManager.SetReplicaMode(ReplicaModeNone)")
	db, err = fbsample.Attach(path)
	fbsample.Check(err)
	defer db.Close()
	fmt.Printf("MON$REPLICA_MODE = %v  (a primary again)\n", replicaMode(db))
	fmt.Println("\ndone.")
}
