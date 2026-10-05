//
// Events.java - Firebird event notification: the three semantics (Java twin
// of ../../../../../events_demo.cpp; see ../../../../../../firebird-events.md).
//
// A listener attachment registers 'demo_event'; a poster attachment runs
// PSQL blocks with POST_EVENT and shows that (1) a ROLLBACK swallows posts,
// (2) delivery happens at COMMIT, and (3) several posts in one transaction
// are delivered once, with a count.  Jaybird implements the auxiliary-channel
// dance in pure Java (op_connect_request, a second socket, op_que_events,
// op_event), like node-firebird and the Go driver.  Its EventManager takes
// two forms: a standalone FBEventManager that opens its own attachment, or -
// used here - EventManager.createFor(connection), which hangs the event
// channel off an existing JDBC connection's attachment.  Like fb-cpp's
// EventListener and the Go driver, it consumes the baseline delivery,
// computes the isc_event_counts delta, and re-queues the one-shot interest
// itself; each delivery reaches an EventListener as a DatabaseEvent
// {name, count} on Jaybird's event thread, handed here to main through a
// BlockingQueue.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Events
//
package fbsamples;

import java.sql.Connection;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

import org.firebirdsql.event.DatabaseEvent;
import org.firebirdsql.event.EventListener;
import org.firebirdsql.event.EventManager;
import org.firebirdsql.event.FBEventManager;

public final class Events {

    private static final String POST = "execute block as begin post_event 'demo_event'; end";

    /** Deliveries that arrive within the wait: {number of deliveries, total count}. */
    private static int[] collect(BlockingQueue<DatabaseEvent> q, long waitMs) throws InterruptedException {
        int deliveries = 0;
        int count = 0;
        DatabaseEvent e = q.poll(waitMs, TimeUnit.MILLISECONDS);
        while (e != null) {
            deliveries++;
            count += e.getEventCount();
            e = q.poll(200, TimeUnit.MILLISECONDS);
        }
        return new int[] {deliveries, count};
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String db = args.length > 0 ? args[0] : "employee";
            boolean pass = true;
            BlockingQueue<DatabaseEvent> deliveries = new LinkedBlockingQueue<>();
            try (Connection listen = FbSample.attach(db, "encoding=NONE");
                    Connection post = FbSample.attach(db, "encoding=NONE");
                    EventManager em = FBEventManager.createFor(listen)) {
                em.connect();
                EventListener listener = deliveries::add;
                em.addEventListener("demo_event", listener);
                System.out.println("listener registered for 'demo_event' "
                        + "(baseline consumed by Jaybird's EventManager)");

                post.setAutoCommit(false);

                // 1. ROLLBACK swallows posts.
                FbSample.execute(post, POST);
                post.rollback();
                int[] r = collect(deliveries, 1500);
                System.out.println("after POST_EVENT + ROLLBACK: delivered count = " + r[1]
                        + "  (" + (r[1] == 0 ? "correct" : "WRONG") + " - rollback swallows posts)");
                pass &= r[1] == 0;

                // 2. Delivery is commit-time; 3. posts coalesce into one count.
                for (int i = 0; i < 3; i++) {
                    FbSample.execute(post, POST);
                }
                System.out.println("3 x POST_EVENT executed, not yet committed - waiting briefly...");
                r = collect(deliveries, 1500);
                System.out.println("before COMMIT: delivered count = " + r[1]
                        + "  (" + (r[1] == 0 ? "correct" : "WRONG") + " - delivery is commit-time)");
                pass &= r[1] == 0;

                post.commit();
                r = collect(deliveries, 5000);
                System.out.println("after COMMIT: " + r[0] + " delivery, count = " + r[1]
                        + "  (" + (r[0] == 1 && r[1] == 3 ? "correct" : "WRONG") + " - one delivery, count 3)");
                pass &= r[0] == 1 && r[1] == 3;

                em.removeEventListener("demo_event", listener);
            }
            System.out.println(pass ? "PASS" : "FAIL");
            if (!pass) {
                System.exit(1);
            }
        });
    }
}
