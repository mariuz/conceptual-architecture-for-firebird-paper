//
// MetadataCache.java - the metadata cache's visibility rule from two
// attachments (Java twin of ../../../../../cpp/metadata_cache.cpp; see
// ../../../../../../metadata-cache.md).
//
//   1. an uncommitted ALTER is visible to its own transaction only;
//   2. a committed ALTER is visible at once, even to a statement prepared
//      inside B's older, still-open SNAPSHOT: metadata is read-committed;
//   3. two concurrent uncommitted DDLs on one object collide in
//      CacheElement::newVersion ("object in use");
//   4. every committed ALTER left a row in RDB$FORMATS.
//
// In JDBC a Connection runs one transaction at a time, so "which prepare
// happened inside which transaction" - the whole experiment - is simply
// which Connection ran it, with auto-commit off.  Jaybird's instructive
// extra is in the errors: the SQLException's message is the whole status
// vector, and its getCause() is an FBSQLExceptionInfo chain (linked by
// getNextException()) with one entry, and one gds code, per status
// element - the vector the libfbclient twins flatten into text, kept as
// objects.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=MetadataCache
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.rows;
import static fbsamples.FbSample.scalar;
import static fbsamples.FbSample.text;

import java.sql.Connection;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;

public final class MetadataCache {

    /** The message without Jaybird's "[SQLState:..., ISC error code:...]" suffix. */
    private static String message(SQLException e) {
        String m = e.getMessage();
        int br = m.lastIndexOf(" [SQLState:");
        return (br >= 0 ? m.substring(0, br) : m).strip();
    }

    /** The gds code of every status-vector element, from the cause chain. */
    private static List<Integer> gdsCodes(SQLException e) {
        List<Integer> codes = new ArrayList<>();
        if (e.getCause() instanceof SQLException info) {
            for (SQLException s = info; s != null; s = s.getNextException()) {
                codes.add(s.getErrorCode());
            }
        }
        return codes;
    }

    private static void tryQuery(String who, Connection con, String sql) {
        try {
            System.out.println(who + ": " + sql + " -> " + text(scalar(con, sql)));
        } catch (SQLException e) {
            System.out.println(who + ": " + sql + " -> ERROR: " + message(e).replace('\n', ' '));
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("mdc", args);
            try (Connection a = attachOrCreate(path); Connection b = attach(path)) {
                execute(a, "recreate table t (a integer)");
                execute(a, "insert into t values (1)");
                a.setAutoCommit(false);
                b.setAutoCommit(false);

                // -- 1. uncommitted DDL: mine, and mine alone ------------------
                System.out.println("== 1. uncommitted ALTER: visible to creator only ==");
                execute(a, "alter table t add e integer");        // stays uncommitted
                tryQuery("A (same tx)  ", a, "select e from t");
                tryQuery("B            ", b, "select e from t");
                b.commit();

                // -- 2. committed DDL ignores open snapshots -------------------
                System.out.println();
                System.out.println("== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==");
                b.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ); // isc_tpb_concurrency
                tryQuery("B (snapshot) ", b, "select count(*) from t");
                a.commit();                                       // E becomes committed
                execute(a, "alter table t add d integer");
                a.commit();                                       // D committed after B's snapshot
                tryQuery("B (same  tx) ", b, "select d from t");
                System.out.println("   (records are snapshot-isolated; metadata is read-committed -");
                System.out.println("    the new statement was prepared against the chain's current head)");
                b.commit();

                // -- 3. concurrent DDL: the newVersion collision ---------------
                System.out.println();
                System.out.println("== 3. two uncommitted DDLs on one object ==");
                execute(a, "alter table t add f integer");
                try {
                    execute(b, "alter table t add g integer");
                    System.out.println("B: ALTER unexpectedly succeeded");
                } catch (SQLException e) {
                    System.out.println("B: ALTER failed:");
                    System.out.println(message(e));
                    System.out.println("   (SQLState " + e.getSQLState() + ", gds codes " + gdsCodes(e) + ")");
                }
                b.rollback();
                a.rollback();                                     // F vanishes with the rollback

                // -- 4. the on-disk half: one format per committed shape --------
                System.out.println();
                System.out.println("== 4. RDB$FORMATS after the committed DDL ==");
                System.out.println("formats stored for T: " + scalar(a, "select count(*) from rdb$formats f "
                        + "join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id "
                        + "where r.rdb$relation_name = 'T'") + " (T has lived through that many shapes)");
                System.out.printf("%-2s %-6s %s%n", "A", "E", "D");
                for (Object[] r : rows(a, "select a, e, d from t")) {
                    System.out.printf("%-2s %-6s %s%n", text(r[0]), text(r[1]), text(r[2]));
                }
                a.commit();
            }
            System.out.println("done.");
        });
    }
}
