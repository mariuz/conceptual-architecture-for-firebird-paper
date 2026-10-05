//
// Migration.java - the type-mapping table made concrete (Java twin of
// ../../../../../cpp/migration.cpp; see
// ../../../../../../migration-and-interoperability.md).
//
// A probe table with the types migrations trip over (INT128, NUMERIC(38,8),
// DECFLOAT(34), TIMESTAMP WITH TIME ZONE, BOOLEAN, CHAR(16) OCTETS as a
// UUID) is inspected on three faces: the DESCRIBED metadata, the native
// values, and the server-side CAST ... AS VARCHAR text face.  A JDBC tool
// sees the describe face only through ResultSetMetaData - a java.sql.Types
// code, a type name, precision/scale and getColumnClassName - which is
// exactly what generic Java migration tools map target types from.  Jaybird
// decodes every type natively and exactly: INT128 / NUMERIC(38,8) / DECFLOAT
// as BigDecimal, TIMESTAMP WITH TIME ZONE as java.time (OffsetDateTime by
// default, ZonedDateTime on request), BOOLEAN as Boolean, OCTETS as byte[].
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Migration
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.nio.ByteBuffer;
import java.sql.Connection;
import java.sql.JDBCType;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.Statement;
import java.time.ZonedDateTime;
import java.util.UUID;

import org.firebirdsql.jdbc.JaybirdTypeCodes;

public final class Migration {

    public static void main(String[] args) {
        FbSample.run(() -> {
            try (Connection con = attachOrCreate(dbPath("migration", args))) {
                execute(con, "RECREATE TABLE TYPE_PROBE ("
                        + "  C_INT128 INT128,"
                        + "  C_NUM    NUMERIC(38,8),"
                        + "  C_DEC    DECFLOAT(34),"
                        + "  C_TSTZ   TIMESTAMP WITH TIME ZONE,"
                        + "  C_BOOL   BOOLEAN,"
                        + "  C_UUID   CHAR(16) CHARACTER SET OCTETS,"
                        + "  C_VC     VARCHAR(20))");
                execute(con, "INSERT INTO TYPE_PROBE VALUES ("
                        + "  170141183460469231731687303715884105727,"
                        + "  123456789012345678901234567890.12345678,"
                        + "  1.234567890123456789012345678901234E+10,"
                        + "  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
                        + "  TRUE, GEN_UUID(), 'naïve ütf8 text')");

                try (Statement st = con.createStatement();
                        ResultSet rs = st.executeQuery("SELECT * FROM TYPE_PROBE")) {
                    // -- 1. the describe face, as JDBC publishes it ---------------
                    ResultSetMetaData md = rs.getMetaData();
                    System.out.println("ResultSetMetaData of SELECT * FROM TYPE_PROBE:\n");
                    System.out.printf("%-8s %-31s %-24s %4s %5s  %s%n",
                            "column", "getColumnType", "getColumnTypeName", "prec", "scale", "getColumnClassName");
                    for (int i = 1; i <= md.getColumnCount(); i++) {
                        System.out.printf("%-8s %-31s %-24s %4d %5d  %s%n", md.getColumnName(i),
                                jdbcName(md.getColumnType(i)) + " (" + md.getColumnType(i) + ")",
                                md.getColumnTypeName(i), md.getPrecision(i), md.getScale(i),
                                md.getColumnClassName(i));
                    }

                    // -- 2. the native face: getObject per column ----------------
                    System.out.println("\nsame row fetched natively (getObject):\n");
                    rs.next();
                    for (int i = 1; i <= md.getColumnCount(); i++) {
                        Object v = rs.getObject(i);
                        String shown = v instanceof byte[] b ? hexUuid(b) : String.valueOf(v);
                        System.out.printf("  %-8s -> %-14s %s%n", md.getColumnName(i),
                                v == null ? "null" : v.getClass().getSimpleName(), shown);
                    }
                    ZonedDateTime z = rs.getObject("C_TSTZ", ZonedDateTime.class);
                    System.out.printf("  %-8s -> %-14s %s   (getObject(col, ZonedDateTime.class))%n",
                            "C_TSTZ", "ZonedDateTime", z);
                }

                // -- 3. the text face: engine-rendered strings ---------------------
                System.out.println("\nthe text face (server-side CAST ... AS VARCHAR):\n");
                for (String c : new String[] {"C_INT128", "C_NUM", "C_DEC", "C_TSTZ", "C_BOOL", "C_VC"}) {
                    System.out.printf("  %-8s = %s%n", c,
                            scalar(con, "SELECT CAST(" + c + " AS VARCHAR(60)) FROM TYPE_PROBE"));
                }
                System.out.printf("  %-8s = %s   (rendered via UUID_TO_CHAR)%n", "C_UUID",
                        scalar(con, "SELECT UUID_TO_CHAR(C_UUID) FROM TYPE_PROBE"));
            }
            System.out.println("\ndone.");
        });
    }

    /** The JDBCType name, or "vendor" for a driver-specific code outside java.sql.Types. */
    private static String jdbcName(int type) {
        try {
            return JDBCType.valueOf(type).getName();
        } catch (IllegalArgumentException e) {
            return type == JaybirdTypeCodes.DECFLOAT ? "Jaybird DECFLOAT" : "vendor";
        }
    }

    private static String hexUuid(byte[] b) {
        ByteBuffer bb = ByteBuffer.wrap(b);
        return new UUID(bb.getLong(), bb.getLong()) + " (" + b.length + " bytes)";
    }
}
