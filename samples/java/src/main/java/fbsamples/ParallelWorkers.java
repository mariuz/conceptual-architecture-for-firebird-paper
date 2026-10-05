//
// ParallelWorkers.java - watching worker attachments appear, and be refused
// (Java twin of ../../../../../cpp/parallel_workers.cpp; see
// ../../../../../../parallel-workers.md).
//
// [A] Against the live server, over Jaybird's pure-Java wire protocol: ask
//     for 4 workers with the connection property parallelWorkers (Jaybird's
//     name for isc_dpb_parallel_workers).  Both knobs are GLOBAL config, so
//     with the stock MaxParallelWorkers = 1 the engine clamps the request
//     and attaches with a status-vector WARNING - which JDBC keeps: it is
//     the Connection's SQLWarning chain (getWarnings()), with its gds code.
// [B] Against an embedded engine (jaybird-native, jdbc:firebird:embedded:)
//     whose private FIREBIRD root sets ParallelWorkers = 4 /
//     MaxParallelWorkers = 8: 200k incompressible rows, CREATE INDEX, and a
//     second embedded attachment polling MON$ATTACHMENTS from a Java thread.
//     The JVM cannot change its own environment through any Java API, so the
//     sample calls libc's setenv through JNA (already on the classpath for
//     jaybird-native) before the engine loads - the C++ sample's setenv,
//     one FFI hop away.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=ParallelWorkers   (~1 min)
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.rows;
import static fbsamples.FbSample.scalar;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.sql.SQLWarning;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import com.sun.jna.Library;
import com.sun.jna.Native;

public final class ParallelWorkers {

    private static final String ROOT = FbSample.SCRATCH + "/fbroot-parallel-java";

    /** libc, for setenv: the JVM exposes no way to change its own environment. */
    public interface LibC extends Library {
        int setenv(String name, String value, int overwrite);
    }

    /** A private $FIREBIRD root: symlinks into the stock install, own firebird.conf. */
    private static void makeRoot() throws IOException {
        Path root = Path.of(ROOT);
        Files.createDirectories(root);
        for (String f : new String[] {"plugins", "intl", "firebird.msg", "tzdata", "plugins.conf", "databases.conf"}) {
            Path link = root.resolve(f);
            if (!Files.exists(link, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
                Files.createSymbolicLink(link, Path.of("/opt/firebird", f));
            }
        }
        Files.writeString(root.resolve("firebird.conf"),
                "ServerMode = Super\nParallelWorkers = 4\nMaxParallelWorkers = 8\n");
    }

    private static String knobs(Connection con) throws SQLException {
        StringBuilder sb = new StringBuilder();
        for (Object[] r : rows(con, "select rdb$config_name, rdb$config_value from rdb$config "
                + "where rdb$config_name in ('ParallelWorkers', 'MaxParallelWorkers') "
                + "order by rdb$config_name desc")) {
            sb.append(sb.isEmpty() ? "" : ", ").append(r[0].toString().strip()).append(" = ").append(r[1]);
        }
        return sb.toString();
    }

    private static Connection embedded(String path) throws SQLException {
        return DriverManager.getConnection("jdbc:firebird:embedded:" + path,
                FbSample.props("createDatabaseIfNotExist=true"));
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String serverDb = dbPath("parallel", args);
            String embeddedDb = args.length > 1 ? args[1] : FbSample.SCRATCH + "/par_embedded_java.fdb";

            // --- [A] the server refuses politely ---------------------------
            attachOrCreate(serverDb).close();
            try (Connection con = FbSample.attach(serverDb, "parallelWorkers=4")) {
                System.out.println("[A] server attach, parallelWorkers=4 (isc_dpb_parallel_workers)");
                for (SQLWarning w = con.getWarnings(); w != null; w = w.getNextWarning()) {
                    System.out.println("    SQLWarning (gds " + w.getErrorCode() + "): " + w.getMessage());
                }
                System.out.println("    server config: " + knobs(con) + "; granted MON$PARALLEL_WORKERS = "
                        + scalar(con, "select mon$parallel_workers from mon$attachments "
                                + "where mon$attachment_id = current_connection")
                        + " -> 0 extra workers\n");
            }

            // --- [B] embedded engine with its own firebird.conf -------------
            makeRoot();
            Native.load("c", LibC.class).setenv("FIREBIRD", ROOT, 1);
            System.setProperty("jna.library.path", "/opt/firebird/lib");
            try (Connection con = embedded(embeddedDb)) {
                System.out.println("[B] embedded attach, FIREBIRD=" + ROOT);
                System.out.println("    engine config: " + knobs(con));
                execute(con, "recreate table parade (id int, val varchar(200))");
                // Incompressible filler: the index build goes parallel only when
                // the relation spans more than one pointer page.
                execute(con, "execute block as declare n int = 0; begin "
                        + "  while (n < 200000) do begin "
                        + "    insert into parade values (:n, "
                        + "      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
                        + "      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
                        + "      uuid_to_char(gen_uuid())); "
                        + "    n = n + 1; "
                        + "  end end");
                System.out.println("    parade table: 200000 rows of 180 incompressible bytes, "
                        + scalar(con, "select count(*) from rdb$pages p join rdb$relations r "
                                + "  on p.rdb$relation_id = r.rdb$relation_id "
                                + "where r.rdb$relation_name = 'PARADE' and p.rdb$page_type = 4")
                        + " pointer pages");

                AtomicInteger maxSeen = new AtomicInteger();
                AtomicBoolean stop = new AtomicBoolean();
                AtomicReference<String> roster = new AtomicReference<>("");
                AtomicReference<Exception> pollError = new AtomicReference<>();
                Thread poller = new Thread(() -> {
                    try (Connection mon = embedded(embeddedDb)) {    // auto-commit: fresh MON$ each query
                        while (!stop.get()) {
                            int n = ((Number) scalar(mon, "select count(*) from mon$attachments "
                                    + "where mon$user = '<Worker>'")).intValue();
                            if (n > maxSeen.get()) {
                                maxSeen.set(n);
                                StringBuilder sb = new StringBuilder();
                                for (Object[] r : rows(mon, "select trim(mon$user), mon$system_flag "
                                        + "from mon$attachments order by mon$attachment_id")) {
                                    sb.append("        ").append(r[0]).append("  (system_flag ").append(r[1]).append(")\n");
                                }
                                roster.set(sb.toString());
                            }
                            Thread.sleep(20);
                        }
                    } catch (SQLException | InterruptedException e) {
                        pollError.set(e);
                    }
                });
                poller.start();

                long t0 = System.nanoTime();
                execute(con, "create index ix_parade on parade (val)");
                long ms = (System.nanoTime() - t0) / 1_000_000;
                stop.set(true);
                poller.join();
                if (pollError.get() != null) {
                    throw pollError.get();
                }
                System.out.println("    create index: " + ms + " ms; max '<Worker>' attachments seen: " + maxSeen.get());
                System.out.print("    MON$ATTACHMENTS at the widest moment:\n" + roster.get());
                System.out.println("    after build: workers stay pooled (idle timeout 60 s): "
                        + scalar(con, "select count(*) from mon$attachments where mon$user = '<Worker>'"));
            }
            System.out.println("done.");
        });
    }
}
