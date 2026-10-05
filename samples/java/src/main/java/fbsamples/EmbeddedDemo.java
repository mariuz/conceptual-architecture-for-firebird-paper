//
// EmbeddedDemo.java - the full server engine, loaded into the JVM (Java twin
// of ../../../../../cpp/embedded_demo.cpp; see
// ../../../../../../embedded-architecture-comparison.md).
//
// Jaybird's default protocol is a pure-Java wire client, but jaybird-native
// adds the EMBEDDED protocol: jdbc:firebird:embedded:<path> loads
// libfbclient through JNA, and a plain local path makes its Y-valve load the
// Engine provider (plugins/libEngine14.so) into this JVM.  The three
// demonstrations of the C++ sample:
//   1. /proc/self/maps before and after: nothing Firebird in the JVM until
//      the first embedded attach, then libfbclient AND libEngine14;
//   2. real work with no server: DDL, DML and a query against a .fdb this
//      process created (NETWORK_PROTOCOL is NULL, the engine's pid is ours);
//   3. the continuum measured: attach+detach timed embedded, through the
//      same libfbclient to inet://localhost/employee, and through Jaybird's
//      own pure-Java wire client - all JDBC, three URLs (and the last once
//      more with employee held open by another attachment, which shows how
//      much of a "remote" attach is the engine opening the database).
// The remote libfbclient leg uses jdbc:firebird:embedded:inet://... rather
// than jdbc:firebird:native:// on purpose: with Jaybird 6.0.6 a second
// libfbclient handle in the same JVM made the exit-time fb_shutdown crash.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=EmbeddedDemo
//
package fbsamples;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.util.List;
import java.util.Properties;

public final class EmbeddedDemo {

    private static String mapped(String fragment) throws IOException {
        return Files.readString(Path.of("/proc/self/maps")).contains(fragment) ? "yes" : "no";
    }

    private static void state(String label) throws IOException {
        System.out.printf("%-15s libfbclient mapped=%s, libEngine14 mapped=%s%n",
                label, mapped("libfbclient"), mapped("libEngine14"));
    }

    private static Connection connect(String url, boolean create) throws SQLException {
        Properties p = FbSample.props(create ? new String[] {"createDatabaseIfNotExist=true"} : new String[0]);
        if (url.endsWith("employee")) {
            p.remove("charSet");
            p.setProperty("encoding", "NONE");
        }
        return DriverManager.getConnection(url, p);
    }

    private static double attachMs(String url) throws SQLException {
        long t0 = System.nanoTime();
        try (Connection c = connect(url, false)) {
            c.isValid(0);
        }
        return (System.nanoTime() - t0) / 1e6;
    }

    public static void main(String[] args) {
        System.setProperty("jna.library.path", "/opt/firebird/lib");
        String local = args.length > 0 ? args[0] : FbSample.SCRATCH + "/embedded_demo_java.fdb";
        String embedded = "jdbc:firebird:embedded:" + local;
        String viaClient = "jdbc:firebird:embedded:inet://" + FbSample.HOST + "/employee";
        String pureJava = "jdbc:firebird://" + FbSample.HOST + "/employee";
        FbSample.run(() -> {
            // --- 1. watch the engine arrive in the JVM --------------------------
            state("before attach:");
            try (Connection con = connect(embedded, true)) {
                state("after  attach:");
                System.out.println();

                // --- 2. real work with no server anywhere ------------------------
                FbSample.execute(con, "recreate table gadgets (id int primary key, name varchar(20))");
                FbSample.execute(con, "insert into gadgets values (1, 'sprocket')");
                FbSample.execute(con, "insert into gadgets values (2, 'flange')");
                FbSample.execute(con, "insert into gadgets values (3, 'grommet')");
                List<Object[]> r = FbSample.rows(con,
                        "select count(*), max(name),"
                        + " coalesce(rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), '<null: in-process>'),"
                        + " a.mon$server_pid"
                        + " from gadgets, mon$attachments a"
                        + " where a.mon$attachment_id = current_connection group by 3, 4");
                Object[] row = r.get(0);
                System.out.println("rows=" + row[0] + "  max(name)=" + FbSample.text(row[1])
                        + "  NETWORK_PROTOCOL=" + FbSample.text(row[2]));
                System.out.println("engine pid=" + row[3] + ", my pid=" + ProcessHandle.current().pid()
                        + " - the 'server' is this JVM");
                System.out.println();

                // --- 3. attach cost, the working attachment still open --------
                String[][] legs = {
                    {"embedded", embedded},
                    {"libfbclient -> server", viaClient},
                    {"pure-Java wire client", pureJava},
                };
                int runs = 5;
                double[] sum = new double[legs.length];
                for (String[] leg : legs) {
                    attachMs(leg[1]);   // warm-up: class loading, JIT, auth code paths
                }
                for (int i = 0; i < runs; i++) {
                    for (int l = 0; l < legs.length; l++) {
                        sum[l] += attachMs(legs[l][1]);
                    }
                }
                System.out.println("attach+detach avg over " + runs + " runs:");
                for (int l = 0; l < legs.length; l++) {
                    System.out.printf("    %-30s %-61s %7.2f ms%n", legs[l][0], legs[l][1], sum[l] / runs);
                }
                // employee was opened cold by every remote attach above; keep it open
                try (Connection keep = connect(pureJava, false)) {
                    keep.isValid(0);
                    double kept = 0;
                    for (int i = 0; i < runs; i++) {
                        kept += attachMs(pureJava);
                    }
                    System.out.printf("    %-30s %-61s %7.2f ms%n", "pure-Java, employee kept open",
                            pureJava, kept / runs);
                }
            }
            System.out.println("done.");
        });
    }
}
