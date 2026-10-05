//
// StmtCache.java - the DSQL statement cache inferred from prepare timings
// (Java twin of ../../../../../cpp/stmt_cache.cpp; see
// ../../../../../../statement-cache.md).
//
// The same four timing runs on a statement that is heavy to COMPILE (a
// six-way self-join) and never executed: identical text (hits), the same
// text plus i trailing spaces (misses: the key is the text verbatim), a
// distinct literal (misses), and identical text after an unrelated
// RECREATE TABLE commit (misses: any DDL commit purges the cache).
// Connection.prepareStatement() is a real prepare-without-execute in
// Jaybird (op_allocate + op_prepare with the describe request) and close()
// frees the handle; the driver keeps no client-side statement cache, so
// nothing stands between the timings and the server's cache.  Two JDBC
// details shape the sample: the prepares run in one manual-commit
// transaction, so run 4's DDL comes from a second attachment (the cache is
// per-database, so another attachment's DDL commit purges this one's
// entries too) - and the JVM's JIT has to warm up first, or run 1 would
// time Jaybird's own class loading and compilation instead of the server.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=StmtCache
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.util.function.IntFunction;

public final class StmtCache {

    private static final int N = 100;
    private static final String HEAVY = "SELECT COUNT(*) FROM t a"
            + " JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id"
            + " JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id"
            + " JOIN t f ON e.id = f.id WHERE a.id > 0";

    private static void prepareOnce(Connection con, String sql) throws SQLException {
        try (PreparedStatement ps = con.prepareStatement(sql)) {
            ps.getMetaData();                        // already described by the prepare
        }
    }

    /** Milliseconds for N prepares of the generated texts, optionally with DDL in between. */
    private static double run(Connection con, Connection ddl, IntFunction<String> text) throws SQLException {
        long total = 0;
        for (int i = 0; i < N; i++) {
            if (ddl != null) {
                execute(ddl, "RECREATE TABLE unrelated (x INT)");   // auto-commit: purges the cache
            }
            String sql = text.apply(i);
            long t0 = System.nanoTime();
            prepareOnce(con, sql);
            total += System.nanoTime() - t0;
        }
        return total / 1e6;
    }

    private static void report(String label, double ms, String verdict) {
        System.out.printf("%-29s %3d prepares: %6.1f ms  (%.2f ms/prepare) - %s%n",
                label, N, ms, ms / N, verdict);
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("stmt_cache", args);
            try (Connection con = attachOrCreate(path); Connection ddl = attach(path)) {
                execute(con, "RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)");
                execute(con, "EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO"
                        + " BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END");
                con.setAutoCommit(false);

                // Warm the JVM (JIT, class loading) on a cheap text, then
                // warm the server's cache with the exact heavy text.
                for (int i = 0; i < 2000; i++) {
                    prepareOnce(con, "SELECT 1 FROM rdb$database");
                }
                prepareOnce(con, HEAVY);

                report("1. identical text", run(con, null, i -> HEAVY), "hits");
                report("2. + i trailing spaces", run(con, null, i -> HEAVY + " ".repeat(i + 1)), "misses");
                report("3. distinct literal", run(con, null, i -> HEAVY.replace("> 0", "> " + i)), "misses");
                report("4. identical text after DDL", run(con, ddl, i -> HEAVY), "misses");
                con.commit();
            }
        });
    }

    private StmtCache() {
    }
}
