//
// Numerics.java - exact and floating numerics on the wire (Java twin of
// ../../../../../cpp/numerics.cpp; see
// ../../../../../../numeric-and-precision-arithmetic.md).
//
// Four experiments: the residue of (0.1 + 0.2) - 0.3 in DOUBLE PRECISION vs
// DECFLOAT(34); a raw fetch of NUMERIC(18,4) showing the scaled integer and
// its scale; INT128 at 2^127-1 and one step past it; the DECFLOAT
// Division_by_zero trap, on by default and then cleared.
//
// Java's standard library has the exact types, and Jaybird uses them:
// NUMERIC, INT128 and DECFLOAT arrive as java.math.BigDecimal, so the
// 2^53 cent that the JS and Rust twins lose survives unless you ask for
// getDouble().  BigDecimal has no Infinity, though: an untrapped DECFLOAT
// 1/0 cannot be fetched as BigDecimal, only as a double or as Jaybird's
// own IEEE decimal type (org.firebirdsql.extern.decimal.Decimal64).  The
// raw fetch goes below JDBC to the GDS-ng FbStatement, whose RowValue holds
// the field bytes exactly as they came off the wire - in XDR big-endian
// order, not the little-endian message buffer the C++ twin prints.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Numerics
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.math.BigDecimal;
import java.math.BigInteger;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

import org.firebirdsql.extern.decimal.Decimal64;
import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.TransactionParameterBuffer;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbStatement;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.gds.ng.fields.FieldDescriptor;
import org.firebirdsql.gds.ng.fields.RowValue;
import org.firebirdsql.gds.ng.listeners.StatementListener;
import org.firebirdsql.jaybird.fb.constants.TpbItems;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class Numerics {

    private static final String ONE_BY_ZERO = "select cast(1 as decfloat(16)) / 0 from rdb$database";

    /** The first line of an engine error, without Jaybird's suffix. */
    private static String brief(SQLException e) {
        String m = e.getMessage();
        int br = m.indexOf(" [SQLState:");
        return br >= 0 ? m.substring(0, br) : m;
    }

    /** Experiment 2: NUMERIC(18,4) through the GDS-ng statement API. */
    private static void rawNumeric(Connection con) throws SQLException {
        FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
        TransactionParameterBuffer tpb = db.createTransactionParameterBuffer();
        tpb.addArgument(TpbItems.isc_tpb_read_committed);
        tpb.addArgument(TpbItems.isc_tpb_rec_version);
        tpb.addArgument(TpbItems.isc_tpb_read);
        FbTransaction tra = db.startTransaction(tpb);
        try {
            FbStatement stmt = db.createStatement(tra);
            try {
                List<RowValue> rows = new ArrayList<>();
                stmt.addStatementListener(new StatementListener() {
                    @Override
                    public void receivedRow(FbStatement sender, RowValue row) {
                        rows.add(row);
                    }
                });
                stmt.prepare("select cast(12345.6789 as numeric(18,4)) from rdb$database");
                FieldDescriptor fd = stmt.getRowDescriptor().getFieldDescriptor(0);
                int type = fd.getType() & ~1;
                System.out.printf("NUMERIC(18,4) wire format: type=%d (%s), length=%d, scale=%d%n",
                        type, type == ISCConstants.SQL_INT64 ? "SQL_INT64" : "?", fd.getLength(), fd.getScale());
                stmt.execute(RowValue.EMPTY_ROW_VALUE);
                stmt.fetchRows(1);
                byte[] bytes = rows.get(0).getFieldData(0);
                StringBuilder hex = new StringBuilder();
                for (byte b : bytes) {
                    hex.append(String.format("%02x ", b));
                }
                long raw = fd.getDatatypeCoder().decodeLong(bytes);
                System.out.println("field bytes (XDR, big-endian)  : " + hex.toString().strip());
                System.out.println("raw integer                    : " + raw);
                System.out.println("value = raw * 10^scale         : " + raw + " * 10^" + fd.getScale() + " = "
                        + BigDecimal.valueOf(raw, -fd.getScale()));
            } finally {
                stmt.close();
            }
        } finally {
            tra.commit();
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("numerics", args);
            try (Connection con = attachOrCreate(path)) {
                // -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3 ------------
                Object dbl = scalar(con, "select (cast(0.1 as double precision) + 0.2) - 0.3 from rdb$database");
                Object dec = scalar(con, "select (cast(0.1 as decfloat(34)) + 0.2) - 0.3 from rdb$database");
                System.out.println("(0.1+0.2)-0.3 in DOUBLE PRECISION : " + dbl + "  ("
                        + dbl.getClass().getSimpleName() + ")");
                System.out.println("(0.1+0.2)-0.3 in DECFLOAT(34)     : " + dec + "  ("
                        + dec.getClass().getSimpleName() + ", signum " + ((BigDecimal) dec).signum() + ")");
                System.out.println();

                // -- 2. NUMERIC(18,4) on the wire --------------------------------
                rawNumeric(con);
                BigDecimal cent = (BigDecimal) scalar(con,
                        "select cast(90071992547409.93 as numeric(18,2)) from rdb$database");
                System.out.println("NUMERIC(18,2) past 2^53        : " + cent
                        + " as BigDecimal, " + String.format("%.2f", cent.doubleValue()) + " as double");
                System.out.println();

                // -- 3. INT128: the full range, and one step past it -------------
                Object max = scalar(con,
                        "select cast(170141183460469231731687303715884105727 as int128) from rdb$database");
                BigInteger expect = BigInteger.TWO.pow(127).subtract(BigInteger.ONE);
                System.out.println("INT128 max  : " + max + "  (" + max.getClass().getSimpleName()
                        + ", == 2^127-1: " + (((BigDecimal) max).toBigIntegerExact().equals(expect)) + ")");
                try {
                    scalar(con, "select cast(170141183460469231731687303715884105727 as int128) + 1 "
                            + "from rdb$database");
                    System.out.println("BUG: overflow not detected");
                } catch (SQLException e) {
                    System.out.println("INT128 max+1: " + brief(e) + " (gds " + e.getErrorCode() + ")");
                }
                System.out.println();

                // -- 4. DECFLOAT division by zero --------------------------------
                try {
                    scalar(con, ONE_BY_ZERO);
                    System.out.println("BUG: default trap did not fire");
                } catch (SQLException e) {
                    System.out.println("1/0 with default traps : " + brief(e) + " (gds " + e.getErrorCode() + ")");
                }
                execute(con, "set decfloat traps to");                   // clear all traps
                try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(ONE_BY_ZERO)) {
                    rs.next();
                    System.out.println("1/0 with traps cleared : getDouble() = " + rs.getDouble(1)
                            + ", getObject(Decimal64.class) = " + rs.getObject(1, Decimal64.class));
                    try {
                        System.out.println("                         getBigDecimal() = " + rs.getBigDecimal(1));
                    } catch (SQLException e) {
                        System.out.println("                         getBigDecimal() fails: " + brief(e));
                    }
                }
            }
            // 4b: the trap setting carried in the DPB (isc_dpb_decfloat_traps)
            try (Connection con = attachOrCreate(dbPath("numerics", args), "decfloatTraps=Inexact");
                    Statement st = con.createStatement(); ResultSet rs = st.executeQuery(ONE_BY_ZERO)) {
                rs.next();
                System.out.println("1/0 with decfloatTraps=Inexact (DPB) : " + rs.getDouble(1));
            }
            System.out.println("done.");
        });
    }
}
