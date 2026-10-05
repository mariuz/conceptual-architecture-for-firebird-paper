// pooling - both directions of connection pooling (Go twin of
// ../../cpp/pooling.cpp; see ../../../connection-pooling.md).
//
// Outbound: the server's external-connections (EDS) pool, tuned with ALTER
// EXTERNAL CONNECTIONS POOL and watched through the EXT_CONN_POOL_*
// context variables while an EXECUTE BLOCK makes three EXECUTE STATEMENT
// ON EXTERNAL calls.  firebirdsql adds a twist: outside an explicit
// transaction its "autocommit" is COMMIT RETAINING, so a statement run on
// the bare *sql.DB never reaches a real commit boundary and the pooled
// external connection stays ACTIVE until the attachment detaches.
//
// Inbound: database/sql IS a client-side pool - every *sql.DB keeps a set
// of driver connections (= attachments) and hands them out per statement.
// The sample sizes one (SetMaxOpenConns 2), exhausts it, watches a third
// request time out and then get the released attachment back, and shows
// what this driver does NOT do on release: it has no ResetSession hook, so
// session state (a USER_SESSION context variable) survives into the next
// borrower - no ALTER SESSION RESET, unlike fb-cpp's pool or the EDS pool.
//
// Run:  go run ./pooling [database] [external-dsn]
package main

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

type querier interface {
	QueryRow(string, ...any) *sql.Row
}

// poolState prints the pool's four SYSTEM context variables.
func poolState(q querier, moment string) {
	var size, life, idle, active string
	fbsample.Check(q.QueryRow(`select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_SIZE'),
	       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_LIFETIME'),
	       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT'),
	       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT')
	  from rdb$database`).Scan(&size, &life, &idle, &active))
	fmt.Printf("%-24s size=%s lifetime=%ss idle=%s active=%s\n", moment, size, life, idle, active)
}

func block(external string) string {
	return `execute block returns (idle varchar(10), active varchar(10)) as
  declare i int = 0;
  declare v int;
begin
  while (i < 3) do
  begin
    execute statement 'select 1 from rdb$database'
      on external '` + external + `'
      as user '` + fbsample.User + `' password '` + fbsample.Password + `'
      into :v;
    i = i + 1;
  end
  idle   = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');
  active = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');
  suspend;
end`
}

func inside(q querier, external, how string) {
	var idle, active string
	fbsample.Check(q.QueryRow(block(external)).Scan(&idle, &active))
	fmt.Printf("%-24s idle=%s active=%s   (3 calls, 1 outbound connection)\n", how, idle, active)
}

func outbound(database, external string) {
	fmt.Println("-- outbound: the server-side EDS pool --")
	db, err := fbsample.Attach(database, "charset=NONE")
	fbsample.Check(err)
	_, err = db.Exec("alter external connections pool set size 5")
	fbsample.Check(err)
	_, err = db.Exec("alter external connections pool set lifetime 30 second")
	fbsample.Check(err)
	poolState(db, "before:")

	// An explicit transaction ends in a real COMMIT.
	tx, err := db.Begin()
	fbsample.Check(err)
	inside(tx, external, "inside the block (tx):")
	fbsample.Check(tx.Commit())
	poolState(db, "after tx.Commit():")

	// The same block on the bare *sql.DB: autocommit = COMMIT RETAINING.
	inside(db, external, "inside (autocommit):")
	poolState(db, "after autocommit:") // still ACTIVE: only retained
	db.Close()                         // detach ends the retained transaction

	db, err = fbsample.Attach(database, "charset=NONE")
	fbsample.Check(err)
	defer db.Close()
	poolState(db, "after detach:")
	_, err = db.Exec("alter external connections pool clear all")
	fbsample.Check(err)
	poolState(db, "after CLEAR ALL:")
}

func stats(db *sql.DB, moment string) {
	s := db.Stats()
	fmt.Printf("%-24s open=%d inUse=%d idle=%d waitCount=%d\n", moment, s.OpenConnections, s.InUse, s.Idle, s.WaitCount)
}

func connID(ctx context.Context, c *sql.Conn) int64 {
	var id int64
	fbsample.Check(c.QueryRowContext(ctx, "select current_connection from rdb$database").Scan(&id))
	return id
}

func inbound(database string) {
	fmt.Println("-- inbound: database/sql's own client-side pool --")
	pool, err := sql.Open("firebirdsql", fbsample.DSN(database, "charset=NONE"))
	fbsample.Check(err)
	defer pool.Close()
	pool.SetMaxOpenConns(2)
	pool.SetMaxIdleConns(2)
	pool.SetConnMaxIdleTime(30 * time.Second)
	ctx := context.Background()

	a, err := pool.Conn(ctx)
	fbsample.Check(err)
	b, err := pool.Conn(ctx)
	fbsample.Check(err)
	idA, idB := connID(ctx, a), connID(ctx, b)
	var set int // RDB$SET_CONTEXT runs when the row is fetched, so Scan it
	fbsample.Check(a.QueryRowContext(ctx, "select rdb$set_context('USER_SESSION', 'BORROWER', 'first') from rdb$database").Scan(&set))
	stats(pool, fmt.Sprintf("took 2 (att %d, %d):", idA, idB))

	short, cancel := context.WithTimeout(ctx, 300*time.Millisecond)
	start := time.Now()
	_, err = pool.Conn(short)
	cancel()
	if errors.Is(err, context.DeadlineExceeded) {
		fmt.Printf("%-24s timed out after %d ms (pool exhausted)\n", "asked for a 3rd:", time.Since(start).Round(100*time.Millisecond).Milliseconds())
	} else {
		fmt.Println("unexpected:", err)
	}

	fbsample.Check(a.Close()) // back to the pool - NOT a detach
	stats(pool, "released one:")
	c, err := pool.Conn(ctx)
	fbsample.Check(err)
	var borrower sql.NullString
	fbsample.Check(c.QueryRowContext(ctx, "select rdb$get_context('USER_SESSION', 'BORROWER') from rdb$database").Scan(&borrower))
	fmt.Printf("%-24s CURRENT_CONNECTION = %d, USER_SESSION BORROWER = %q\n", "3rd ask served:", connID(ctx, c), borrower.String)
	if borrower.Valid {
		fmt.Println("                         (same attachment, previous borrower's session state intact: no reset on release)")
	} else {
		fmt.Println("                         (session state was reset on release)")
	}
	c.Close()
	b.Close()

	var n int
	fbsample.Check(pool.QueryRow("select count(*) from mon$attachments where mon$remote_pid = ?", os.Getpid()).Scan(&n))
	stats(pool, "all released:")
	fmt.Printf("%-24s %d of this process's attachments still open in MON$ATTACHMENTS (kept idle by the pool)\n", "", n)
}

func main() {
	database, external := "employee", "inet://localhost/employee"
	if len(os.Args) > 1 {
		database = os.Args[1]
	}
	if len(os.Args) > 2 {
		external = os.Args[2]
	}
	outbound(database, external)
	inbound(database)
	fmt.Println("done.")
}
