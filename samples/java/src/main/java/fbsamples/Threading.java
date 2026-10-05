//
// Threading.java - SuperServer's thread-per-attachment topology from the
// outside (Java twin of ../../../../../cpp/threading.cpp; see
// ../../../../../../threading-and-synchronization.md).
//
// The same census: MON$SERVER_PID names the engine process, /proc/<pid>/task
// counts its threads before, during and after twelve concurrent attachments
// from twelve client threads, and MON$ATTACHMENTS shows the Cache Writer
// and Garbage Collector as system attachments.  The workers are an
// ExecutorService of twelve platform threads, each opening its own
// Connection; a CountDownLatch (the Python twin's semaphore, the Go twin's
// WaitGroup) takes the "during" census only once all twelve have attached
// and queried.  The Java rule sits between the twins': a Jaybird Connection
// IS thread-safe - every call takes the attachment's lock - but sharing one
// only serializes the threads on that lock and on one server worker, so
// one connection per thread (or a pool) is still the design.  Monitoring
// runs in JDBC auto-commit: each query is its own transaction, hence a
// fresh MON$ snapshot.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Threading
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.rows;
import static fbsamples.FbSample.scalar;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.stream.Stream;

public final class Threading {

    private static final int WORKERS = 12;

    private static long countThreads(Object pid) throws IOException {
        try (Stream<Path> s = Files.list(Path.of("/proc", pid.toString(), "task"))) {
            return s.count();
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("threading", args);
            // Jaybird sends no isc_dpb_process_name unless asked (the processName
            // property or the org.firebirdsql.jdbc.processName system property):
            // the workers leave MON$REMOTE_PROCESS NULL, this attachment names itself.
            try (Connection con = attachOrCreate(path, "processName=fbsamples.Threading")) {
                execute(con, "recreate table t (id int primary key, v int)");
                execute(con, "update or insert into t values (1, 0) matching (id)");
                Object pid = scalar(con, "select mon$server_pid from mon$attachments"
                        + " where mon$attachment_id = current_connection");
                System.out.println("engine process: pid " + pid + ", " + countThreads(pid)
                        + " threads (1 attachment open)");

                CountDownLatch attached = new CountDownLatch(WORKERS);
                CountDownLatch release = new CountDownLatch(1);
                ExecutorService pool = Executors.newFixedThreadPool(WORKERS);
                List<Future<Object>> results = new ArrayList<>();
                try {
                    for (int i = 0; i < WORKERS; i++) {
                        results.add(pool.submit(() -> {
                            try (Connection w = attach(path)) {   // one attachment per thread
                                Object n = scalar(w, "select count(*) from t");
                                attached.countDown();
                                release.await();
                                return n;
                            }
                        }));
                    }
                    attached.await();
                    System.out.println("with 12 extra attachments: " + countThreads(pid) + " threads | "
                            + scalar(con, "select count(*) from mon$attachments where mon$system_flag = 0")
                            + " user attachments, "
                            + scalar(con, "select count(distinct mon$server_pid) from mon$attachments")
                            + " distinct server pid");
                } finally {
                    release.countDown();
                    pool.shutdown();
                }
                for (Future<Object> f : results) {
                    f.get();                                 // surface any worker failure
                }
                Thread.sleep(1000);
                System.out.println("after they detach:        " + countThreads(pid)
                        + " threads (pooled, not destroyed)");

                System.out.printf("  %2s %3s  %-18s %s%n", "ID", "SYS", "USER", "REMOTE_PROCESS");
                for (Object[] r : rows(con, "select mon$attachment_id, mon$system_flag, trim(mon$user),"
                        + " coalesce(mon$remote_process, iif(mon$system_flag = 1, '<internal>', '<null>'))"
                        + " from mon$attachments order by mon$attachment_id")) {
                    System.out.printf("  %2s %3s  %-18s %s%n", r[0], r[1], r[2], r[3]);
                }
            }
        });
    }

    private Threading() {
    }
}
