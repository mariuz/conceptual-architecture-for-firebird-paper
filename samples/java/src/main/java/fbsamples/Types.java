//
// Types.java - the headline Firebird types through JDBC (Java twin of
// ../../../../../cpp/types.cpp; see ../../../../../../sql-dialect-and-types.md).
//
// The same showcase table: BOOLEAN, INT128 at its maximum, DECFLOAT(34)
// holding an exact 0.1, TIMESTAMP WITH TIME ZONE with a named zone, and a
// CHECK-constrained domain.  Three faces of each column: the wire type code
// (from Jaybird's own GDS-ng layer - FbStatement.getRowDescriptor(), the
// FieldDescriptor Jaybird decodes the XDR with), the public JDBC metadata
// (java.sql.Types, Firebird type name, Java class), and the fetched value.
// Java sits high on the ladder with no extra dependency: BigDecimal and
// BigInteger are in the JDK, so INT128 arrives as an exact BigDecimal
// (getObject(col, BigInteger.class) works too), DECFLOAT(34) as a
// BigDecimal equal to new BigDecimal("0.1"), BOOLEAN as Boolean; and
// TIMESTAMP WITH TIME ZONE as an OffsetDateTime by default - the region
// name only via getObject(col, ZonedDateTime.class) (see Temporal.java).
// The domain violation is an SQLException with SQLState and gds code.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Types
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.math.BigDecimal;
import java.math.BigInteger;
import java.sql.Connection;
import java.sql.JDBCType;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.time.ZonedDateTime;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbStatement;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.gds.ng.fields.RowDescriptor;
import org.firebirdsql.jdbc.FirebirdConnection;
import org.firebirdsql.jdbc.JaybirdTypeCodes;

public final class Types {

    private static String wireName(int t) {
        return switch (t) {
            case ISCConstants.SQL_BOOLEAN -> "SQL_BOOLEAN";
            case ISCConstants.SQL_INT128 -> "SQL_INT128";
            case ISCConstants.SQL_DEC34 -> "SQL_DEC34";
            case ISCConstants.SQL_TIMESTAMP_TZ -> "SQL_TIMESTAMP_TZ";
            case ISCConstants.SQL_VARYING -> "SQL_VARYING";
            default -> "(other)";
        };
    }

    /** A java.sql.Types code by name; Jaybird's own codes (JaybirdTypeCodes) are not JDBCType values. */
    private static String jdbcType(int code) {
        try {
            return JDBCType.valueOf(code).getName();
        } catch (IllegalArgumentException vendorCode) {
            return code == JaybirdTypeCodes.DECFLOAT ? "JaybirdTypeCodes.DECFLOAT (" + code + ")" : "vendor " + code;
        }
    }

    /** Wire type codes, from a statement prepared (never executed) on the GDS-ng layer. */
    private static RowDescriptor describe(Connection con, String sql) throws SQLException {
        FirebirdConnection fc = con.unwrap(FirebirdConnection.class);
        FbDatabase db = fc.getFbDatabase();
        FbTransaction tx = db.startTransaction(fc.getTransactionParameters(Connection.TRANSACTION_READ_COMMITTED));
        try {
            FbStatement st = db.createStatement(tx);
            try {
                st.prepare(sql);
                return st.getRowDescriptor();
            } finally {
                st.close();
            }
        } finally {
            tx.commit();
        }
    }

    private static void quietly(Connection con, String sql) {
        try {
            execute(con, sql);
        } catch (SQLException ignored) {
            // did not exist
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("types", args);
            try (Connection con = attachOrCreate(path)) {
                // Auto-commit: each drop is committed before the next (DFW).
                quietly(con, "DROP TABLE showcase");
                quietly(con, "DROP DOMAIN d_email");
                execute(con, "CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')");
                execute(con, "CREATE TABLE showcase ("
                        + "  flag  BOOLEAN,"
                        + "  big   INT128,"
                        + "  money DECFLOAT(34),"
                        + "  born  TIMESTAMP WITH TIME ZONE,"
                        + "  mail  d_email)");
                execute(con, "INSERT INTO showcase VALUES ("
                        + "  TRUE,"
                        + "  170141183460469231731687303715884105727,"
                        + "  0.1,"
                        + "  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
                        + "  'user@example.com')");

                try {
                    execute(con, "INSERT INTO showcase (mail) VALUES ('not-an-address')");
                    System.out.println("BUG: domain CHECK did not fire");
                } catch (SQLException e) {
                    System.out.println("domain CHECK rejected 'not-an-address':");
                    System.out.println("    SQLState " + e.getSQLState() + ", gds " + e.getErrorCode() + ": "
                            + e.getMessage().split("\n")[0]);
                }

                RowDescriptor rd = describe(con, "SELECT * FROM showcase");
                System.out.println();
                System.out.printf("%-6s %-24s %-51s %s%n", "column", "wire type (FbStatement)",
                        "JDBC getColumnType / getColumnTypeName", "getColumnClassName");
                System.out.printf("%-6s %-24s %-51s %s%n", "------", "-----------------------",
                        "---------------------------------------------------", "------------------");
                try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery("SELECT * FROM showcase")) {
                    ResultSetMetaData md = rs.getMetaData();
                    for (int i = 1; i <= md.getColumnCount(); i++) {
                        int wire = rd.getFieldDescriptor(i - 1).getType() & ~1;
                        System.out.printf("%-6s %-24s %-51s %s%n", md.getColumnName(i),
                                wire + " = " + wireName(wire),
                                jdbcType(md.getColumnType(i)) + " / " + md.getColumnTypeName(i),
                                md.getColumnClassName(i));
                    }

                    rs.next();
                    BigDecimal big = rs.getBigDecimal("BIG");
                    BigDecimal money = rs.getBigDecimal("MONEY");
                    BigInteger max = BigInteger.ONE.shiftLeft(127).subtract(BigInteger.ONE);
                    System.out.println();
                    System.out.println("typed round-trip:");
                    System.out.println("  FLAG  " + rs.getObject("FLAG") + "  ("
                            + rs.getObject("FLAG").getClass().getSimpleName() + ")");
                    System.out.println("  BIG   " + big + "  == 2^127 - 1 (BigInteger) ? "
                            + rs.getObject("BIG", BigInteger.class).equals(max));
                    System.out.println("  MONEY " + money + "  == new BigDecimal(\"0.1\") ? "
                            + (money.compareTo(new BigDecimal("0.1")) == 0)
                            + "  (and new BigDecimal(0.1d)? " + (money.compareTo(new BigDecimal(0.1d)) == 0) + ")");
                    System.out.println("  BORN  " + rs.getObject("BORN") + "  ("
                            + rs.getObject("BORN").getClass().getSimpleName() + "); as ZonedDateTime: "
                            + rs.getObject("BORN", ZonedDateTime.class));
                    System.out.println("  MAIL  \"" + rs.getString("MAIL") + "\"");
                }
            }
            System.out.println();
            System.out.println("done.");
        });
    }

    private Types() {
    }
}
