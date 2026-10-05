// events - Firebird event notification: the three defining semantics (Go
// twin of ../../events_demo.cpp; see ../../../firebird-events.md).
//
// A LISTENER registers interest in 'demo_event'; a POSTER runs PSQL blocks
// with POST_EVENT, showing that (1) a ROLLBACK swallows posts, (2) delivery
// happens at COMMIT, not when POST_EVENT executes, and (3) several posts in
// one transaction coalesce into one delivery with a count.
//
// firebirdsql implements the whole auxiliary-channel dance in Go, with no
// client library: firebirdsql.NewFBEvent(dsn) is the event hub, and each
// Subscribe / SubscribeChan opens its OWN attachment, sends
// op_connect_request, dials the auxiliary port the server returns, queues
// the interest with op_que_events and - on every op_event - re-queues the
// one-shot interest itself.  Like isc_event_counts it hands over deltas,
// not node-firebird's raw running counter: it subtracts the previous
// counts and the engine's +1, and drops zero deltas, so the baseline
// delivery never reaches the channel.  The deliveries arrive as
// firebirdsql.Event values on a Go channel, so "wait briefly" is a select
// with a timer.
//
// Run:  go run ./events [database]      (default: employee)
package main

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/nakagami/firebirdsql"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const event = "demo_event"

// delivered sums the counts that arrive within timeout.
func delivered(ch <-chan firebirdsql.Event, timeout time.Duration) int {
	total := 0
	deadline := time.After(timeout)
	for {
		select {
		case e := <-ch:
			if e.Name == event {
				total += e.Count
			}
		case <-deadline:
			return total
		}
	}
}

// post runs POST_EVENT n times in one explicit transaction, left open.
func post(poster *sql.DB, n int) *sql.Tx {
	tx, err := poster.BeginTx(context.Background(), nil)
	fbsample.Check(err)
	_, err = tx.Exec("EXECUTE BLOCK AS BEGIN " +
		strings.Repeat(fmt.Sprintf("POST_EVENT '%s'; ", event), n) + "END")
	fbsample.Check(err)
	return tx
}

func verdict(ok bool, good string) string {
	if ok {
		return "(correct - " + good + ")"
	}
	return "(UNEXPECTED)"
}

func main() {
	database := "employee"
	if len(os.Args) > 1 {
		database = os.Args[1]
	}
	poster, err := fbsample.Attach(database, "charset=NONE")
	fbsample.Check(err)
	defer poster.Close()

	hub, err := firebirdsql.NewFBEvent(fbsample.DSN(database, "charset=NONE"))
	fbsample.Check(err)
	defer hub.Close()
	ch := make(chan firebirdsql.Event, 16)
	sub, err := hub.SubscribeChan([]string{event}, ch)
	fbsample.Check(err)
	defer sub.Unsubscribe()
	fmt.Printf("listener subscribed to '%s' (baseline consumed by the driver's delta filter)\n", event)
	ok := true

	// 1. POST_EVENT then ROLLBACK: nothing may be delivered.
	fbsample.Check(post(poster, 1).Rollback())
	got := delivered(ch, 1500*time.Millisecond)
	ok = ok && got == 0
	fmt.Printf("after POST_EVENT + ROLLBACK: delivered count = %d  %s\n", got,
		verdict(got == 0, "rollback swallows posts"))

	// 2. Three POST_EVENTs in one transaction, then COMMIT: one delivery, count 3.
	tx := post(poster, 3)
	fmt.Println("3 x POST_EVENT executed, not yet committed - waiting briefly...")
	early := delivered(ch, time.Second)
	ok = ok && early == 0
	fmt.Printf("before COMMIT: delivered count = %d  %s\n", early,
		verdict(early == 0, "delivery is commit-time"))

	fbsample.Check(tx.Commit())
	var deliveries, total int
	deadline := time.After(3 * time.Second)
collect:
	for {
		select {
		case e := <-ch:
			deliveries++
			total += e.Count
		case <-deadline:
			break collect
		}
	}
	ok = ok && total == 3 && deliveries == 1
	fmt.Printf("after COMMIT: %d delivery, count = %d  %s\n", deliveries, total,
		verdict(total == 3 && deliveries == 1, "one delivery, count 3"))
	if ok {
		fmt.Println("PASS")
	} else {
		fmt.Println("FAIL")
		os.Exit(1)
	}
}
