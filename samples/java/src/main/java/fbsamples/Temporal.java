//
// Temporal.java - WITH TIME ZONE storage and the session zone (Java twin of
// ../../../../../cpp/temporal.cpp; see
// ../../../../../../temporal-and-time-zones.md).
//
// The same four steps: the wire struct of a named-zone and an offset
// TIMESTAMP WITH TIME ZONE literal (UTC instant + 2-byte zone id), a DST
// conversion, instant equality, and SET TIME ZONE.  Jaybird reaches the
// raw bytes the C++ sample memcpy's out of its message buffer through its
// own GDS-ng layer: FbStatement + a StatementListener hand back each
// RowValue's field data exactly as it came off the wire (XDR: big-endian,
// 12 bytes, the 2-byte zone id padded to 4), and Jaybird's
// TimeZoneMapping - its compiled-in copy of Firebird's zone id table -
// names the id without a round trip.  Through JDBC the type has
// two Java faces: getObject() gives an OffsetDateTime (the region name is
// lost), getObject(col, ZonedDateTime.class) keeps it.  And unlike the
// other drivers, Jaybird always sends isc_dpb_session_time_zone: by default
// the JVM's zone, so the session zone follows the client, not the server
// (sessionTimeZone=server opts out).
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Temporal
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.nio.ByteBuffer;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.time.OffsetDateTime;
import java.time.ZoneId;
import java.time.ZonedDateTime;
import java.util.ArrayList;
import java.util.List;
import java.util.TimeZone;

import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbStatement;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.gds.ng.fields.RowValue;
import org.firebirdsql.gds.ng.listeners.StatementListener;
import org.firebirdsql.gds.ng.tz.TimeZoneMapping;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class Temporal {

    private static final String LITERALS =
            "select timestamp '2026-07-18 12:00:00 America/New_York',"
            + " timestamp '2026-07-18 12:00:00 -05:00' from rdb$database";

    /** One row fetched through the wire-level API: raw field bytes. */
    private static RowValue rawRow(Connection con) throws SQLException {
        FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
        List<RowValue> rows = new ArrayList<>();
        FbTransaction tx = db.startTransaction(con.unwrap(FirebirdConnection.class)
                .getTransactionParameters(Connection.TRANSACTION_READ_COMMITTED));
        try {
            FbStatement st = db.createStatement(tx);
            try {
                st.addStatementListener(new StatementListener() {
                    @Override
                    public void receivedRow(FbStatement sender, RowValue row) {
                        rows.add(row);
                    }
                });
                st.prepare(LITERALS);
                st.execute(RowValue.EMPTY_ROW_VALUE);
                st.fetchRows(1);
            } finally {
                st.close();
            }
        } finally {
            tx.commit();
        }
        return rows.get(0);
    }

    private static String session(Connection con) throws SQLException {
        return String.format("session zone: %-18s CURRENT_TIMESTAMP: %s",
                scalar(con, "select rdb$get_context('SYSTEM', 'SESSION_TIMEZONE') from rdb$database"),
                scalar(con, "select cast(current_timestamp as varchar(50)) from rdb$database"));
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("temporal", args);
            try (Connection con = attachOrCreate(path)) {
                // -- 1./2. The wire struct of both literals.
                RowValue raw = rawRow(con);
                TimeZoneMapping zones = TimeZoneMapping.getInstance();
                try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(LITERALS)) {
                    rs.next();
                    for (int i = 0; i < 2; i++) {
                        byte[] b = raw.getFieldData(i);
                        ByteBuffer bb = ByteBuffer.wrap(b);       // XDR: big-endian
                        int days = bb.getInt();
                        int time = bb.getInt();
                        int zone = bb.getInt() & 0xFFFF;          // a USHORT, XDR-padded to 4 bytes
                        ZoneId named = zones.timeZoneById(zone);
                        System.out.println((i == 0 ? "named-zone" : "offset") + " literal:");
                        System.out.printf("  on the wire : UTC days=%d time=%d  zone id=%d  (%d bytes)%n",
                                days, time, zone, b.length);
                        System.out.println("  id -> zone  : " + named + "  (Jaybird's TimeZoneMapping)");
                        System.out.println("  getObject() : " + rs.getObject(i + 1)
                                + "  (" + rs.getObject(i + 1).getClass().getSimpleName() + ")");
                        System.out.println("  as Zoned    : " + rs.getObject(i + 1, ZonedDateTime.class));
                    }
                    // The default face really is OffsetDateTime:
                    OffsetDateTime odt = rs.getObject(1, OffsetDateTime.class);
                    System.out.println("instant      : " + odt.toInstant());
                }

                // -- 3. DST: the same NY wall time, two UTC instants.
                System.out.println();
                System.out.println("NY 12:00 in UTC, winter: " + scalar(con,
                        "select cast(timestamp '2026-01-18 12:00:00 America/New_York'"
                                + " at time zone 'Etc/UTC' as varchar(50)) from rdb$database"));
                System.out.println("NY 12:00 in UTC, summer: " + scalar(con,
                        "select cast(timestamp '2026-07-18 12:00:00 America/New_York'"
                                + " at time zone 'Etc/UTC' as varchar(50)) from rdb$database"));
                System.out.println("10:00 -02:00 = 09:00 -03:00 ? " + scalar(con,
                        "select iif(time '10:00:00 -02:00' = time '09:00:00 -03:00',"
                                + " 'EQUAL', 'different') from rdb$database").toString().trim());

                // -- 4. The session zone: Jaybird's default is the JVM's zone.
                System.out.println();
                System.out.println("JVM default zone: " + ZoneId.systemDefault());
                System.out.println(session(con) + "   <- Jaybird default");
                execute(con, "set time zone 'Asia/Tokyo'");
                System.out.println(session(con) + "   <- SET TIME ZONE");
            }
            // The default really is the client's zone: move the JVM, not the server.
            TimeZone jvm = TimeZone.getDefault();
            TimeZone.setDefault(TimeZone.getTimeZone("Australia/Sydney"));
            try (Connection moved = attach(path)) {
                System.out.println(session(moved) + "   <- JVM zone set to Australia/Sydney");
            } finally {
                TimeZone.setDefault(jvm);
            }
            try (Connection server = attach(path, "sessionTimeZone=server")) {
                System.out.println(session(server) + "   <- sessionTimeZone=server");
            }
            try (Connection dpb = attach(path, "sessionTimeZone=America/Sao_Paulo")) {
                System.out.println(session(dpb) + "   <- sessionTimeZone=America/Sao_Paulo");
            }
            System.out.println();
            System.out.println("done.");
        });
    }

    private Temporal() {
    }
}
