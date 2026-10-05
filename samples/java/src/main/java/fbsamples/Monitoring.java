//
// Monitoring.java - the MON$ hierarchy and its stable snapshot (Java twin of
// ../../../../../cpp/monitoring.cpp; see ../../../../../../monitoring-and-tuning.md).
//
// Walks MON$DATABASE -> MON$ATTACHMENTS -> MON$TRANSACTIONS ->
// MON$STATEMENTS down to this attachment, then runs a 10 000-row full scan
// inside the same transaction and shows its own MON$STAT_ID-joined counters
// frozen - the first MON$ select took a stable snapshot - until a new
// transaction refreshes them.  JDBC's trap is the JavaScript one: with
// auto-commit on, every statement is its own transaction and the freeze
// would never appear, so the sample turns it off (and asks for
// REPEATABLE_READ, Firebird's SNAPSHOT).  What a pure-wire client usually
// lacks, Jaybird has: the *live* channel next to MON$, the attachment info
// items isc_info_fetches and isc_info_read_seq_count (per-relation
// sequential reads) over op_info_database via getFbDatabase().getDatabaseInfo
// - answered by the engine at the moment of the call, so they move while
// the MON$ row stands still.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Monitoring
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;
import static fbsamples.FbSample.text;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.VaxEncoding;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class Monitoring {

    private static final String COUNTERS =
            "SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS, R.MON$RECORD_INSERTS, "
            + "       I.MON$PAGE_FETCHES, I.MON$PAGE_READS "
            + "FROM MON$ATTACHMENTS A "
            + "JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID "
            + "JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID "
            + "WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION";

    /** Print every row as name=value pairs (column labels from the metadata). */
    private static void show(Connection con, String label, String sql) throws SQLException {
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(sql)) {
            ResultSetMetaData md = rs.getMetaData();
            while (rs.next()) {
                StringBuilder sb = new StringBuilder(String.format("%-39s", label));
                for (int i = 1; i <= md.getColumnCount(); i++) {
                    sb.append(' ').append(md.getColumnLabel(i).replace("MON$", "").replace("RECORD_", "").toLowerCase())
                            .append('=').append(text(rs.getObject(i)).strip());
                }
                System.out.println(sb);
            }
        }
    }

    /** The live channel: page fetches and one relation's sequential reads, as info items. */
    private static void liveInfo(Connection con, String label, int relationId) throws SQLException {
        FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
        byte[] buf = db.getDatabaseInfo(new byte[] {
            ISCConstants.isc_info_fetches, ISCConstants.isc_info_read_seq_count, ISCConstants.isc_info_end}, 1024);
        long fetches = 0;
        long seq = 0;
        for (int pos = 0; buf[pos] != ISCConstants.isc_info_end;) {
            int item = buf[pos];
            int len = VaxEncoding.iscVaxInteger2(buf, pos + 1);
            int start = pos + 3;
            if (item == ISCConstants.isc_info_fetches) {
                fetches = VaxEncoding.iscVaxLong(buf, start, len);
            } else if (item == ISCConstants.isc_info_read_seq_count) {
                // (2-byte relation id, 4-byte count) pairs while counts fit in 32 bits
                for (int p = start; p + 6 <= start + len; p += 6) {
                    if (VaxEncoding.iscVaxInteger2(buf, p) == relationId) {
                        seq = VaxEncoding.iscVaxInteger(buf, p + 2, 4);
                    }
                }
            }
            pos = start + len;
        }
        System.out.printf("%-39s isc_info_fetches=%d  MON_WORK sequential reads=%d%n", label, fetches, seq);
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            try (Connection con = attachOrCreate(dbPath("monitoring", args))) {
                execute(con, "RECREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)");
                execute(con, "EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
                        + "  WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); I = I + 1; END "
                        + "END");
                int relId = ((Number) scalar(con,
                        "SELECT RDB$RELATION_ID FROM RDB$RELATIONS WHERE RDB$RELATION_NAME = 'MON_WORK'")).intValue();

                // One explicit SNAPSHOT transaction: auto-commit would refresh every read.
                con.setAutoCommit(false);
                con.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ);

                // -- 1. the hierarchy, one consistent snapshot ------------------
                show(con, "MON$DATABASE:", "SELECT MON$OLDEST_TRANSACTION AS OIT, MON$OLDEST_ACTIVE AS OAT, "
                        + "MON$NEXT_TRANSACTION AS NEXT, MON$PAGE_BUFFERS FROM MON$DATABASE");
                show(con, "attachment -> transaction -> statement:",
                        "SELECT A.MON$ATTACHMENT_ID AS ATT, TRIM(A.MON$USER) AS USR, T.MON$TRANSACTION_ID AS TX, "
                        + "       S.MON$STATE, CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 30) AS VARCHAR(30)) AS SQL_HEAD "
                        + "FROM MON$ATTACHMENTS A "
                        + "JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID "
                        + "JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID "
                        + "WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION");

                // -- 2. the snapshot property, against the live info channel ----
                show(con, "MON$ snapshot 1:", COUNTERS);
                liveInfo(con, "info items, before the workload:", relId);
                System.out.println("... running workload: SELECT COUNT(*) full scan + indexed lookup ...");
                System.out.println("count = " + scalar(con, "SELECT COUNT(*) FROM MON_WORK")
                        + ", point = " + scalar(con, "SELECT VAL FROM MON_WORK WHERE ID = 4242"));
                show(con, "same transaction: STILL snapshot 1:", COUNTERS);
                liveInfo(con, "info items, same moment: live:", relId);
                con.commit();
                show(con, "new transaction: fresh snapshot:", COUNTERS);
                con.commit();
            }
            System.out.println("done.");
        });
    }
}
