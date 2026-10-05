//
// MemoryPools.java - the pool hierarchy made visible from SQL via
// MON$MEMORY_USAGE (Java twin of ../../../../../cpp/memory_pools.cpp; see
// ../../../../../../memory-management.md).
//
// The per-level summary with the parent-redirection signature (child pools
// with real MON$MEMORY_USED and zero MON$MEMORY_ALLOCATED), the worker's
// database -> attachment -> transaction chain, and a transaction pool
// growing under an uncommitted 3000-row UPDATE and dying with its
// rollback - watched from a second attachment, because a MON$ snapshot is
// frozen per transaction.  Jaybird is a pure-Java wire client, yet like the
// libfbclient-based Python twin it gets the ids from *info calls* rather
// than SQL: FbDatabase.getDatabaseInfo with isc_info_attachment_id and
// isc_info_current_memory (the wire op_info_database), and the running
// transaction's FbTransaction.getTransactionId() (isc_info_tra_id) - the
// latter reached through FBConnection.getGDSHelper(), the one step past the
// public JDBC/Firebird interfaces.  The roll-up arithmetic is done here.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=MemoryPools
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.VaxEncoding;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.jdbc.FBConnection;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class MemoryPools {

    /** One numeric database-info item over op_info_database. */
    private static long dbInfo(Connection con, int item) throws SQLException {
        FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
        byte[] buf = db.getDatabaseInfo(new byte[] {(byte) item, ISCConstants.isc_info_end}, 64);
        if (buf[0] != item) {
            throw new SQLException("unexpected info item " + buf[0]);
        }
        int len = VaxEncoding.iscVaxInteger2(buf, 1);
        return VaxEncoding.iscVaxLong(buf, 3, len);
    }

    private static void levelSummary(Connection mon) throws SQLException {
        System.out.println("GROUP POOLS USED       ALLOCATED  WITH_OWN_EXTENTS");
        try (Statement st = mon.createStatement(); ResultSet rs = st.executeQuery(
                "select MON$STAT_GROUP, count(*), sum(MON$MEMORY_USED), "
                + "       sum(MON$MEMORY_ALLOCATED), count(nullif(MON$MEMORY_ALLOCATED, 0)) "
                + "from MON$MEMORY_USAGE group by 1 order by 1")) {
            while (rs.next()) {
                // SUM over BIGINT is INT128 on Firebird 4+: Jaybird maps it to BigDecimal.
                System.out.printf("%-5d %-5d %-10s %-10s %d%n", rs.getInt(1), rs.getInt(2),
                        rs.getBigDecimal(3).toPlainString(), rs.getBigDecimal(4).toPlainString(), rs.getInt(5));
            }
        }
        mon.commit();
    }

    /** One pool's used/allocated, from a fresh monitor snapshot. */
    private static long poolRow(Connection mon, String label, String join, long id) throws SQLException {
        long used = -1;
        try (PreparedStatement ps = mon.prepareStatement(
                "select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE " + join)) {
            if (id >= 0) {
                ps.setLong(1, id);
            }
            try (ResultSet rs = ps.executeQuery()) {
                if (rs.next()) {
                    used = rs.getLong(1);
                    System.out.printf("  %-24s used=%-10d allocated=%d%n", label, used, rs.getLong(2));
                }
            }
        }
        mon.commit();       // next read is a new snapshot
        return used;
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("memory_pools", args);
            try (Connection worker = attachOrCreate(path); Connection mon = attach(path)) {
                execute(worker, "recreate table t (id int, pad varchar(200))");
                execute(worker, "execute block as declare i int = 0; begin"
                        + "  while (i < 3000) do begin"
                        + "    insert into t values (:i, rpad('x', 200, 'x')); i = i + 1;"
                        + "  end "
                        + "end");
                mon.setAutoCommit(false);

                System.out.println("-- per-level summary (0=db 1=att 2=tra 3=stmt 5=cmp; "
                        + "used > 0 with allocated = 0: parent redirection)");
                levelSummary(mon);

                // The worker's own chain; ids from info calls, not SQL.
                worker.setAutoCommit(false);
                execute(worker, "select 1 from rdb$database");      // make the transaction start
                long att = dbInfo(worker, ISCConstants.isc_info_attachment_id);
                long tra = worker.unwrap(FBConnection.class)
                        .getGDSHelper().getCurrentTransaction().getTransactionId();

                System.out.printf("%n-- worker's pool chain (attachment %d, transaction %d; before the update)%n",
                        att, tra);
                poolRow(mon, "database pool:", "join MON$DATABASE using (MON$STAT_ID)", -1);
                System.out.println("  (isc_info_current_memory " + dbInfo(worker, ISCConstants.isc_info_current_memory) + ")");
                String attJoin = "join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ?";
                String traJoin = "join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = ?";
                poolRow(mon, "worker attachment pool:", attJoin, att);
                poolRow(mon, "worker transaction pool:", traJoin, tra);

                // Grow the transaction pool: the undo log lives in it.
                execute(worker, "update t set pad = rpad('y', 200, 'y')");
                System.out.println("\n-- after an uncommitted 3000-row UPDATE in that transaction");
                long attBefore = poolRow(mon, "worker attachment pool:", attJoin, att);
                long traUsed = poolRow(mon, "worker transaction pool:", traJoin, tra);

                worker.rollback();      // bulk-free: the whole pool goes at once
                System.out.println("\n-- after rollback (transaction pool destroyed with its undo log)");
                long attAfter = poolRow(mon, "worker attachment pool:", attJoin, att);
                System.out.printf("  attachment used fell by %d; the dead transaction pool held %d%n",
                        attBefore - attAfter, traUsed);
            }
        });
    }
}
