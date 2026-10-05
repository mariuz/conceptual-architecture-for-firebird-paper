//
// ApiStyles.java - one query through the API levels of one Java driver
// (Java twin of ../../../../../cpp/api_styles.cpp; see
// ../../../../../../client-apis-and-drivers.md).
//
// The C++ sample runs one SELECT through both C APIs of libfbclient (the
// legacy ISC API and the OO API).  Jaybird is the Java row of the driver
// table, and it is BOTH strategies at once:
//
//   1. JDBC over PURE_JAVA - the portable surface, typed getString();
//   2. the GDS-ng layer underneath (FirebirdConnection.getFbDatabase()) -
//      Jaybird's own wire-level API, closest to the C++ descriptor work:
//      prepare, a RowDescriptor (SQL type 449 = SQL_VARYING + 1 for nullable, its length),
//      execute, fetchRows, and raw column bytes delivered to a
//      StatementListener; plus executeImmediate with no statement handle;
//   3. JDBC over NATIVE (jdbc:firebird:native://) - jaybird-native maps
//      libfbclient with JNA and drives it through the legacy ISC API
//      (isc_attach_database, isc_dsql_prepare, XSQLDA ...), the same calls
//      as the C++ sample's first half, behind the same JDBC interfaces;
//   4. the Services API, which the driver also speaks itself;
//   5. the error model: a status vector becomes one SQLException with the
//      first GDS code as getErrorCode() and the SQLSTATE.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=ApiStyles
//
package fbsamples;

import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.util.Properties;

import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbStatement;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.gds.ng.fields.FieldDescriptor;
import org.firebirdsql.gds.ng.fields.RowValue;
import org.firebirdsql.gds.ng.listeners.StatementListener;
import org.firebirdsql.jdbc.FirebirdConnection;
import org.firebirdsql.management.FBServiceManager;

public final class ApiStyles {

    private static final String SQL =
            "select rdb$get_context('SYSTEM', 'ENGINE_VERSION') from rdb$database";

    private static Properties props() {
        Properties p = FbSample.props();
        p.remove("charSet");
        p.setProperty("encoding", "NONE");
        return p;
    }

    private static String layer(Connection con) throws SQLException {
        return con.unwrap(FirebirdConnection.class).getFbDatabase().getClass().getSimpleName();
    }

    public static void main(String[] args) {
        System.setProperty("jna.library.path", "/opt/firebird/lib");
        String database = args.length > 0 ? args[0] : "employee";
        String pure = "jdbc:firebird://" + FbSample.HOST + "/" + database;
        String nativ = "jdbc:firebird:native://" + FbSample.HOST + "/" + database;
        FbSample.run(() -> {
            // -- 1. JDBC, pure Java ---------------------------------------------
            try (Connection con = DriverManager.getConnection(pure, props())) {
                System.out.println("[JDBC, PURE_JAVA     ] engine version = " + FbSample.scalar(con, SQL)
                        + "   (FbDatabase: " + layer(con) + ")");

                // -- 2. the GDS-ng layer of the same attachment ----------------------
                FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
                FbTransaction tx = db.startTransaction("set transaction read only");
                try {
                    FbStatement st = db.createStatement(tx);
                    try {
                        st.prepare(SQL);
                        FieldDescriptor fd = st.getRowDescriptor().getFieldDescriptor(0);
                        st.addStatementListener(new StatementListener() {
                            @Override
                            public void receivedRow(FbStatement sender, RowValue row) {
                                byte[] raw = row.getFieldData(0);
                                System.out.println("[GDS-ng FbStatement  ] engine version = "
                                        + new String(raw, StandardCharsets.US_ASCII)
                                        + "   (sqltype " + fd.getType() + ", length " + fd.getLength()
                                        + ", " + raw.length + " raw bytes)");
                            }
                        });
                        st.execute(RowValue.EMPTY_ROW_VALUE);
                        st.fetchRows(10);
                    } finally {
                        st.close();
                    }
                    db.executeImmediate("set decfloat round half_even", tx);
                    System.out.println("[GDS-ng FbDatabase   ] executeImmediate(\"set decfloat round half_even\") ok");
                } finally {
                    tx.commit();
                }
            }

            // -- 3. JDBC over libfbclient's ISC API --------------------------------
            try (Connection con = DriverManager.getConnection(nativ, props())) {
                System.out.println("[JDBC, NATIVE        ] engine version = " + FbSample.scalar(con, SQL)
                        + "   (FbDatabase: " + layer(con) + ", isc_* calls via JNA)");
            }

            // -- 4. the Services API ---------------------------------------------
            FBServiceManager sm = new FBServiceManager();
            sm.setServerName(FbSample.HOST);
            sm.setUser(FbSample.USER);
            sm.setPassword(FbSample.PASSWORD);
            System.out.println("[service_mgr         ] server version = " + sm.getServerVersion().getRawVersions().get(0));

            // -- 5. the error model ----------------------------------------------
            try (Connection con = DriverManager.getConnection(
                    "jdbc:firebird://" + FbSample.HOST + "//nonexistent/x.fdb", props())) {
                System.out.println("unexpected: attached");
            } catch (SQLException e) {
                System.out.println("[error model         ] attach /nonexistent/x.fdb -> "
                        + e.getClass().getSimpleName());
                System.out.println("    " + FbSample.errorText(e).replace("\n", "\n    "));
                System.out.println("    getErrorCode() " + e.getErrorCode() + "  getSQLState() " + e.getSQLState());
            }
            System.out.println("one driver: its own wire protocol AND libfbclient's ISC API. done.");
        });
    }
}
