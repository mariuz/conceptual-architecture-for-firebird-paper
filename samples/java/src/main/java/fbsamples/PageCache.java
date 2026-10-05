//
// PageCache.java - one hot-page ping-pong, two cache topologies (Java twin
// of ../../../../../cpp/page_cache.cpp; see
// ../../../../../../page-cache-coherency.md).
//
//   phase 1 - two client processes -> ONE SuperServer shared cache
//   phase 2 - two EMBEDDED engine processes with PRIVATE caches over one
//             file (ServerMode=SuperClassic sandbox): coherency by LCK_bdb
//             page locks and blocking ASTs, so data travels through the disk
//
// Rows 1 and 2 share a data page; each worker commits 300 updates to its own
// row, then reads its MON$IO_STATS.  The parent only launches child JVMs.
//
// Jaybird has both kinds of driver in one jar set: phase 1 uses the default
// PURE_JAVA wire protocol (jdbc:firebird://localhost/...), like node-firebird
// and the Go driver, and phase 2 switches to the EMBEDDED protocol of
// jaybird-native (jdbc:firebird:embedded:/path), which loads libfbclient
// through JNA - and with it the engine - into the child JVM.  The protocol
// is just the URL prefix.  The children are `java -cp <our classpath>`
// processes started with ProcessBuilder, whose environment() carries
// FIREBIRD=<sandbox> so the in-process engine reads the SuperClassic
// firebird.conf.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=PageCache
//
package fbsamples;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;

public final class PageCache {

    private static final String SRV_URL = FbSample.url(FbSample.SCRATCH + "/page_cache_srv_java.fdb");
    private static final Path EMB_DB = Path.of(FbSample.SCRATCH, "page_cache_emb_java.fdb");
    private static final String EMB_URL = "jdbc:firebird:embedded:" + EMB_DB;
    private static final Path SANDBOX = Path.of(FbSample.SCRATCH, "fbemb_java");
    private static final int ROUNDS = 300;

    private static Connection connect(String url, boolean create) throws SQLException {
        return create
                ? DriverManager.getConnection(url, FbSample.props("createDatabaseIfNotExist=true"))
                : DriverManager.getConnection(url, FbSample.props());
    }

    private static void init(String url) throws SQLException {          // --init
        try (Connection con = connect(url, true)) {
            FbSample.execute(con, "recreate table t (id int primary key, v int)");
            FbSample.execute(con, "insert into t values (1, 0)");
            FbSample.execute(con, "insert into t values (2, 0)");
        }
    }

    private static void worker(String url, String rowId) throws SQLException {   // --worker
        try (Connection con = connect(url, false)) {
            for (int i = 0; i < ROUNDS; i++) {                             // auto-commit: one tx each
                FbSample.execute(con, "update t set v = v + 1 where id = " + rowId);
            }
            Object[] io = FbSample.rows(con,
                    "select mon$page_fetches, mon$page_reads, mon$page_writes "
                    + "from mon$io_stats join mon$attachments using (mon$stat_id) "
                    + "where mon$attachment_id = current_connection").get(0);
            System.out.printf("  worker pid %-6d row %s: %d commits | page fetches=%-6s reads=%-4s writes=%s%n",
                    ProcessHandle.current().pid(), rowId, ROUNDS, io[0], io[1], io[2]);
        }
    }

    private static void check(String url) throws SQLException {          // --check
        try (Connection con = connect(url, false)) {
            for (Object[] r : FbSample.rows(con, "select id, v from t order by id")) {
                System.out.printf("  final: id=%s v=%s (expected %d)%n", r[0], r[1], ROUNDS);
            }
        }
    }

    /** Start this class again in a child JVM with a role argument. */
    private static Process spawn(String firebirdRoot, String... args) throws IOException {
        List<String> cmd = new ArrayList<>(List.of(
                Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                "-cp", System.getProperty("java.class.path"), PageCache.class.getName()));
        cmd.addAll(List.of(args));
        ProcessBuilder pb = new ProcessBuilder(cmd).inheritIO();
        if (firebirdRoot != null) {
            pb.environment().put("FIREBIRD", firebirdRoot);
        }
        return pb.start();
    }

    private static void await(Process... ps) throws InterruptedException {
        for (Process p : ps) {
            if (p.waitFor() != 0) {
                throw new IllegalStateException("child exited with " + p.exitValue());
            }
        }
    }

    private static void phase(String url, String firebirdRoot) throws Exception {
        await(spawn(firebirdRoot, "--init", url));
        await(spawn(firebirdRoot, "--worker", url, "1"), spawn(firebirdRoot, "--worker", url, "2"));
        await(spawn(firebirdRoot, "--check", url));
    }

    /** A FIREBIRD root whose firebird.conf says SuperClassic. */
    private static void buildSandbox() throws IOException {
        Files.createDirectories(SANDBOX);
        for (String f : new String[] {"plugins", "intl", "tzdata", "firebird.msg", "security6.fdb"}) {
            Path link = SANDBOX.resolve(f);
            if (!Files.exists(link, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
                Files.createSymbolicLink(link, Path.of("/opt/firebird", f));
            }
        }
        Files.writeString(SANDBOX.resolve("firebird.conf"), "ServerMode = SuperClassic\n");
    }

    public static void main(String[] args) {
        // jaybird-native finds libfbclient through JNA's search path.
        System.setProperty("jna.library.path", "/opt/firebird/lib");
        FbSample.run(() -> {
            if (args.length > 1) {
                switch (args[0]) {
                    case "--init" -> init(args[1]);
                    case "--worker" -> worker(args[1], args[2]);
                    case "--check" -> check(args[1]);
                    default -> throw new IllegalArgumentException(args[0]);
                }
                return;
            }
            System.out.println("phase 1: two client processes, ONE SuperServer shared cache");
            phase(SRV_URL, null);

            buildSandbox();
            Files.deleteIfExists(EMB_DB);
            System.out.println("phase 2: two EMBEDDED engine processes, PRIVATE page caches");
            phase(EMB_URL, SANDBOX.toString());
            System.out.println("same workload - the private caches paid for coherency in disk I/O.");
        });
    }
}
