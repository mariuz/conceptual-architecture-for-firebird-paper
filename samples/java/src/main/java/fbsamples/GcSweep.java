//
// GcSweep.java - record versions, the collectors and sweep, watched through
// MON$RECORD_STATS (Java twin of ../../../../../cpp/gc_sweep.cpp; see
// ../../../../../../garbage-collection-and-sweep.md).
//
// On a fresh database: pin a SNAPSHOT, commit twelve updates under it,
// release it and scan (intermediate GC, purge), delete and scan (expunge),
// then roll back a no_auto_undo transaction to freeze the OIT - and sweep.
// Jaybird covers every lever the C++ sample uses and the one it can only
// recommend: the no_auto_undo stump is a TPB item installed behind a JDBC
// isolation level (FirebirdConnection.setTransactionParameters), the four
// header counters are database info items over op_info_database
// (isc_info_oldest_transaction / _active / _snapshot / next_transaction,
// so peeking starts no transaction), and the sweep is
// FBMaintenanceManager.sweepDatabase() - the Services API's gfix -sweep,
// run inside the server.  All from a pure-Java wire client.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=GcSweep
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.recreate;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.TransactionParameterBuffer;
import org.firebirdsql.gds.VaxEncoding;
import org.firebirdsql.jaybird.fb.constants.TpbItems;
import org.firebirdsql.jdbc.FirebirdConnection;
import org.firebirdsql.management.FBMaintenanceManager;

public final class GcSweep {

    /** MON$ is a snapshot per transaction: each auto-commit query is a fresh one. */
    private static void showStats(Connection con, String label) throws SQLException {
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(
                "select r.MON$RECORD_UPDATES, r.MON$RECORD_IMGC, r.MON$RECORD_PURGES, "
                + "       r.MON$RECORD_EXPUNGES, r.MON$BACKVERSION_READS "
                + "from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)")) {
            rs.next();
            System.out.printf("%-34s upd=%-4d imgc=%-3d purges=%-3d expunges=%-3d backreads=%d%n",
                    label, rs.getLong(1), rs.getLong(2), rs.getLong(3), rs.getLong(4), rs.getLong(5));
        }
    }

    /** The header counters as database info items: no transaction needed. */
    private static void showCounters(Connection con, String label) throws SQLException {
        int[] items = {ISCConstants.isc_info_oldest_transaction, ISCConstants.isc_info_oldest_active,
            ISCConstants.isc_info_oldest_snapshot, ISCConstants.isc_info_next_transaction,
            ISCConstants.isc_info_sweep_interval};
        byte[] req = new byte[items.length + 1];
        for (int i = 0; i < items.length; i++) {
            req[i] = (byte) items[i];
        }
        req[items.length] = ISCConstants.isc_info_end;
        byte[] buf = con.unwrap(FirebirdConnection.class).getFbDatabase().getDatabaseInfo(req, 128);
        long[] v = new long[items.length];
        for (int pos = 0; buf[pos] != ISCConstants.isc_info_end;) {
            int item = buf[pos] & 0xff;
            int len = VaxEncoding.iscVaxInteger2(buf, pos + 1);
            long value = VaxEncoding.iscVaxLong(buf, pos + 3, len);
            for (int i = 0; i < items.length; i++) {
                if (items[i] == item) {
                    v[i] = value;
                }
            }
            pos += 3 + len;
        }
        System.out.printf("%-34s OIT=%d OAT=%d OST=%d Next=%d (sweep interval %d)%n",
                label, v[0], v[1], v[2], v[3], v[4]);
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("gc_sweep", args);
            try (Connection writer = recreate(path); Connection pinner = attach(path)) {
                execute(writer, "create table gctest (id int primary key, val int)");
                execute(writer, "insert into gctest values (1, 0)");

                // 1. Pin a snapshot (REPEATABLE_READ is isc_tpb_concurrency).
                pinner.setAutoCommit(false);
                pinner.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ);
                String read = "select val from gctest where id = 1";
                System.out.println("pinned SNAPSHOT reads val = " + scalar(pinner, read));
                showStats(writer, "before updates:");

                // 2. Twelve committed updates (auto-commit: one transaction each).
                try (PreparedStatement ps = writer.prepareStatement("update gctest set val = ? where id = 1")) {
                    for (int i = 1; i <= 12; i++) {
                        ps.setInt(1, i);
                        ps.executeUpdate();
                    }
                }
                showStats(writer, "after 12 updates (snapshot open):");
                System.out.println("pinned SNAPSHOT still reads val = " + scalar(pinner, read));

                // 3. Release the snapshot; a scan trips over the below-OST chain.
                pinner.commit();
                System.out.println("snapshot released; new reader sees val = " + scalar(writer, read));
                Thread.sleep(1500);
                showStats(writer, "after release + scan + 1.5s:");

                // 4. A committed DELETE older than the OST is expunged, not purged.
                execute(writer, "delete from gctest where id = 1");
                scalar(writer, "select count(*) from gctest");
                Thread.sleep(1500);
                showStats(writer, "after DELETE + scan + 1.5s:");

                // 5. A no_auto_undo rollback leaves a stump that pins the OIT.
                showCounters(writer, "header counters before rollback:");
                FirebirdConnection fc = writer.unwrap(FirebirdConnection.class);
                TransactionParameterBuffer stump = fc.createTransactionParameterBuffer();
                stump.addArgument(TpbItems.isc_tpb_concurrency);
                stump.addArgument(TpbItems.isc_tpb_write);
                stump.addArgument(TpbItems.isc_tpb_nowait);
                stump.addArgument(TpbItems.isc_tpb_no_auto_undo);
                fc.setTransactionParameters(Connection.TRANSACTION_SERIALIZABLE, stump);
                writer.setAutoCommit(false);
                writer.setTransactionIsolation(Connection.TRANSACTION_SERIALIZABLE);
                execute(writer, "insert into gctest values (2, 0)");
                writer.rollback();
                writer.setAutoCommit(true);
                showCounters(writer, "after no_auto_undo rollback:");

                // 6. The sweep the C++ sample can only recommend.
                FBMaintenanceManager mm = new FBMaintenanceManager();
                mm.setServerName(FbSample.HOST);
                mm.setUser(FbSample.USER);
                mm.setPassword(FbSample.PASSWORD);
                mm.setDatabase(path);
                mm.sweepDatabase();
                showCounters(writer, "after sweepDatabase():");
            }
        });
    }
}
