//
// Trace.java - a user trace session driven through the Services API (Java
// twin of ../../../../../cpp/trace.cpp; see ../../../../../../trace-and-audit.md).
//
// The same choreography: service A starts a session whose configuration
// targets one database and streams the TraceLog back; a worker attaches and
// runs one marker query; service B stops the session, which ends A's stream.
// Jaybird has the trace family as a class, org.firebirdsql.management.
// FBTraceManager, and it owns more of the choreography than any other
// wrapper: startTraceSession(name, config) sends isc_action_svc_trace_start
// with the text in isc_spb_trc_cfg and returns at once - the manager drains
// the stream on a thread of its OWN into the OutputStream given to
// setLogger(); stopTraceSession(id) and listTraceSessions() each open a
// fresh service attachment (service B).  The catch is in the drain: it asks
// for isc_info_svc_to_eof, not isc_info_svc_line, and the server answers
// that in bursts - the "Trace session ID n started" line is held back until
// the first traced events come with it.  getSessionId(name), which sniffs
// that line out of the stream, is therefore null until traced work starts
// (a first draft that polled it for 5 s before running the worker never
// saw it).  The sample gets the id from the server instead
// (listTraceSessions, matched by name) and stops the session in a finally:
// until the session ends, the manager's non-daemon thread keeps the JVM
// alive.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Trace
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.scalar;

import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.sql.Connection;

import org.firebirdsql.management.FBTraceManager;

public final class Trace {

    private static final long T0 = System.nanoTime();
    private static final String NAME = "hands-on-java";

    private static long ms() {
        return (System.nanoTime() - T0) / 1_000_000;
    }

    /**
     * An OutputStream that prints whole lines under a prefix, and marks each
     * burst of output (bytes after a pause of 200 ms or more) with its time.
     */
    private static final class Prefixed extends OutputStream {
        private final String prefix;
        private final StringBuilder captured;
        private final ByteArrayOutputStream line = new ByteArrayOutputStream();
        private long last = -1_000;

        Prefixed(String prefix, StringBuilder captured) {
            this.prefix = prefix;
            this.captured = captured;
        }

        @Override
        public synchronized void write(int b) {
            long now = ms();
            if (now - last >= 200) {
                System.out.printf("%s ---- output arrives at +%d ms ----%n", prefix, now);
            }
            last = now;
            if (b == '\n') {
                String s = line.toString(StandardCharsets.UTF_8).stripTrailing();
                line.reset();
                if (captured != null) {
                    captured.append(s).append('\n');
                }
                if (!s.isEmpty()) {
                    System.out.printf("%s %s%n", prefix, s);
                }
            } else {
                line.write(b);
            }
        }
    }

    /** The id of the named session, from isc_action_svc_trace_list output. */
    private static Integer idFromList(String list, String name) {
        Integer current = null;
        for (String l : list.split("\n")) {
            String t = l.strip();
            if (t.startsWith("Session ID:")) {
                current = Integer.valueOf(t.substring("Session ID:".length()).strip());
            } else if (t.startsWith("name:") && t.substring(5).strip().equals(name)) {
                return current;
            }
        }
        return null;
    }

    private static FBTraceManager manager(OutputStream logger) {
        FBTraceManager m = new FBTraceManager();
        m.setServerName(FbSample.HOST);
        m.setUser(FbSample.USER);
        m.setPassword(FbSample.PASSWORD);
        m.setLogger(logger);
        return m;
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("trace", args);
            try (Connection c = attachOrCreate(path)) {
                c.isValid(1);                        // the observed database must exist first
            }
            String cfg = "database = " + path + "\n"
                    + "{\n"
                    + "  enabled = true\n"
                    + "  log_connections = true\n"
                    + "  log_transactions = true\n"
                    + "  log_statement_finish = true\n"
                    + "  print_plan = true\n"
                    + "  print_perf = true\n"
                    + "  time_threshold = 0\n"
                    + "}\n";

            // -- service A: start; FBTraceManager drains on its own thread.
            FBTraceManager a = manager(new Prefixed("[trace]", null));
            a.startTraceSession(NAME, cfg);
            System.out.printf("[main ] +%d ms startTraceSession(\"%s\") returned%n", ms(), NAME);

            Integer id = null;
            try {
                Thread.sleep(1000);
                System.out.println("[main ] getSessionId(\"" + NAME + "\") = " + a.getSessionId(NAME));

                // -- the observed side.
                try (Connection w = attach(path)) {
                    System.out.printf("[worker] +%d ms marker query says: %s%n", ms(),
                            scalar(w, "SELECT COUNT(*) FROM RDB$RELATIONS /* traced! */"));
                }
                Thread.sleep(1500);

                // -- service B: the id from the server's own list.
                StringBuilder list = new StringBuilder();
                manager(new Prefixed("[list ]", list)).listTraceSessions();
                id = idFromList(list.toString(), NAME);
            } finally {
                if (id != null) {
                    System.out.printf("[main ] +%d ms stopTraceSession(%d)%n", ms(), id);
                    manager(new Prefixed("[stop ]", null)).stopTraceSession(id);
                }
            }
            Thread.sleep(1000);                      // let A's thread flush the end of the stream
            System.out.println("done.");
        });
    }

    private Trace() {
    }
}
