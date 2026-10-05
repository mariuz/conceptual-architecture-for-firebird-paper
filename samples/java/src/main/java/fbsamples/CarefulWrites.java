//
// CarefulWrites.java - kill a database engine mid-write; the file needs no
// recovery (Java twin of ../../../../../cpp/careful_writes.cpp; see
// ../../../../../../careful-writes-and-crash-safety.md).
//
// Like the C++ twin this uses the EMBEDDED engine, so the process that is
// killed IS the engine: Jaybird's jdbc:firebird:embedded: protocol
// (jaybird-native) loads libfbclient through JNA, and a plain local path
// makes its Y-valve load the Engine provider into the JVM.  Java cannot
// fork(), so the parent starts a second JVM running this class with
// --writer (ProcessBuilder, same java binary and class path).  The writer
// creates the database, commits a marker row, then bulk-inserts 500,000
// rows in a transaction it never commits.  The parent watches the .fdb grow
// (the engine flushing pages of the UNCOMMITTED transaction), sends SIGKILL
// with Process.destroyForcibly(), re-attaches embedded and counts:
// committed rows all present, uncommitted rows all gone, no log replay -
// because there is no log.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=CarefulWrites
//
package fbsamples;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.util.Properties;

public final class CarefulWrites {

    private static Connection embedded(String path, boolean create) throws SQLException {
        Properties p = FbSample.props(create ? new String[] {"createDatabaseIfNotExist=true"} : new String[0]);
        return DriverManager.getConnection("jdbc:firebird:embedded:" + path, p);
    }

    /** Child mode: a JVM that is the engine; commits a marker, then never commits. */
    private static void writer(String path) throws Exception {
        long pid = ProcessHandle.current().pid();
        try (Connection con = embedded(path, true)) {
            FbSample.execute(con, "create table cw (id int, tag varchar(30))");
            FbSample.execute(con, "insert into cw values (1, 'committed-marker')");
            System.out.println("[writer " + pid + "] marker row committed (embedded engine in this JVM)");

            con.setAutoCommit(false);    // the crash victim: never committed
            FbSample.execute(con,
                    "execute block as declare i int = 0; begin"
                    + "  while (i < 500000) do begin"
                    + "    insert into cw values (:i + 1000, 'uncommitted'); i = i + 1;"
                    + "  end "
                    + "end");
            System.out.println("[writer] bulk insert finished uncommitted; waiting for SIGKILL");
            Thread.sleep(Long.MAX_VALUE);
        }
    }

    public static void main(String[] args) {
        System.setProperty("jna.library.path", "/opt/firebird/lib");
        boolean childMode = args.length > 0 && args[0].equals("--writer");
        String path = childMode ? args[1]
                : (args.length > 0 ? args[0] : FbSample.SCRATCH + "/careful_writes_java.fdb");
        FbSample.run(() -> {
            if (childMode) {
                writer(path);
                return;
            }
            Path file = Path.of(path);
            Files.deleteIfExists(file);     // fresh run

            // 1. Spawn the writer: a second JVM running the embedded engine.
            String java = ProcessHandle.current().info().command().orElse("java");
            Process child = new ProcessBuilder(java,
                    "-cp", System.getProperty("java.class.path"),
                    CarefulWrites.class.getName(), "--writer", path)
                    .inheritIO().start();

            // 2. Wait until the file is visibly growing, then kill -9 the engine.
            long base = -1;
            for (int i = 0; i < 1200; i++) {
                Thread.sleep(50);
                long sz = Files.exists(file) ? Files.size(file) : -1;
                if (base < 0 && sz > 0) {
                    base = sz;
                }
                if (base > 0 && sz > base + 2 * 1024 * 1024) {
                    System.out.println("file grew " + base + " -> " + sz
                            + " bytes; SIGKILL to engine pid " + child.pid());
                    break;
                }
            }
            child.destroyForcibly();    // SIGKILL on Unix
            child.waitFor();

            // 3. Re-attach at once: there is no recovery step to run.
            long t0 = System.nanoTime();
            Object committed;
            Object uncommitted;
            try (Connection con = embedded(path, false)) {
                committed = FbSample.scalar(con, "select count(*) from cw where tag = 'committed-marker'");
                uncommitted = FbSample.scalar(con, "select count(*) from cw where tag = 'uncommitted'");
            }
            long ms = (System.nanoTime() - t0) / 1_000_000;
            System.out.println("re-attach + both counts took " + ms + " ms");
            System.out.println("committed marker rows : " + committed + "   <- survived the crash");
            System.out.println("uncommitted rows      : " + uncommitted
                    + "   <- rolled back by visibility, not replay");
            System.out.println("(" + new File(path).length() + " bytes on disk after the crash)");
        });
    }
}
