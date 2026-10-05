// trace - a complete user trace session through the Services API (Go twin
// of ../../cpp/trace.cpp; see ../../../trace-and-audit.md).
//
//	service A: trace start with an inline configuration, then the session's
//	           TraceLog streamed back line by line;
//	worker:    attaches to the traced database and runs one marker query;
//	service B: lists the active sessions, then stops ours, ending A's stream.
//
// firebirdsql has a TraceManager that owns this whole lifecycle:
// StartWithName sends isc_action_svc_trace_start with the text in
// isc_spb_trc_cfg and parses the "Trace session ID n started" reply into
// TraceSession.ID(); WaitStrings drains the stream into a Go channel; List
// returns isc_action_svc_trace_list as raw text (no parser, so Firebird 6's
// new "plugins:" line is no problem); Stop opens its OWN service attachment
// for isc_action_svc_trace_stop and checks the reply names our session.
// So "service B" is the manager's doing, and the worker is a goroutine.
//
// Run:  go run ./trace [database]
package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
	"github.com/nakagami/firebirdsql"
)

const traceCfg = `database = %s
{
  enabled = true
  log_connections = true
  log_transactions = true
  log_statement_finish = true
  print_plan = true
  print_perf = true
  time_threshold = 0
}
`

func main() {
	path := fbsample.DBPath("trace")
	db, err := fbsample.Create(path) // must exist before the session
	fbsample.Check(err)
	db.Close()

	tm, err := firebirdsql.NewTraceManager(fbsample.ServiceAddr(), fbsample.User,
		fbsample.Password, fbsample.ServiceOptions())
	fbsample.Check(err)
	session, err := tm.StartWithName("hands-on-go", fmt.Sprintf(traceCfg, path))
	fbsample.Check(err)
	defer session.Close()
	fmt.Printf("[trace] Trace session ID %d started\n", session.ID())

	// The observed side, then the list and the stop from other services.
	go func() {
		time.Sleep(800 * time.Millisecond)
		w, err := fbsample.Attach(path)
		fbsample.Check(err)
		tx, err := w.Begin()
		fbsample.Check(err)
		var n int
		fbsample.Check(tx.QueryRow("SELECT COUNT(*) FROM RDB$RELATIONS /* traced! */").Scan(&n))
		fbsample.Check(tx.Commit())
		w.Close()
		fmt.Println("[worker] marker query says:", n)

		time.Sleep(1200 * time.Millisecond)
		list, err := tm.List()
		fbsample.Check(err)
		for _, l := range strings.Split(strings.TrimSpace(list), "\n") {
			fmt.Println("[list ]", strings.TrimRight(l, " \r"))
		}
		fbsample.Check(session.Stop())
		fmt.Printf("[stop ] Trace session ID %d stopped\n", session.ID())
	}()

	// Service A's stream: the trace output itself, until the stop ends it.
	lines := make(chan string)
	streamErr := make(chan error, 1)
	go func() { streamErr <- session.WaitStrings(lines) }()
	for {
		select {
		case l := <-lines:
			for _, s := range strings.Split(strings.TrimRight(l, "\n"), "\n") {
				if strings.TrimSpace(s) != "" {
					fmt.Println("[trace]", strings.TrimRight(s, " \r"))
				}
			}
			continue
		case err := <-streamErr:
			fbsample.Check(err)
		}
		break
	}
	fmt.Println("done.")
}
