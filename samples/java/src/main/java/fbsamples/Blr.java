//
// Blr.java - stored BLR read raw from the catalog, and the BLR a wire
// client writes itself (Java twin of ../../../../../cpp/blr.cpp; see
// ../../../../../../blr-intermediate-language.md).
//
// Reads the computed column EMPLOYEE.FULL_NAME (RDB$FIELDS.RDB$COMPUTED_BLR)
// and the procedure GET_EMP_PROJ (RDB$PROCEDURES.RDB$PROCEDURE_BLR) from the
// stock employee database, hex-dumps them and decodes the opening bytes
// like the C++ sample: the whole expression tree of the computed column,
// the message declarations at the head of the procedure.
//
// Fetching is ResultSet.getBytes(): Jaybird reads the sub_type 2 blob's
// segments itself.  The opcodes come from Jaybird's own
// org.firebirdsql.gds.BlrConstants (a transcription of blr.h the driver
// needs), and that is the instructive part: like the Go driver, PURE_JAVA
// Jaybird has no libfbclient to describe parameter rows for it, so it
// WRITES BLR on every execute.  The third part prepares a statement on the
// GDS-ng layer and asks the driver's DefaultBlrCalculator for the message
// BLR it would send, then decodes it with the same decoder as the stored
// procedure's messages.
//
// Read-only against employee.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Blr
//
package fbsamples;

import static org.firebirdsql.gds.BlrConstants.blr_begin;
import static org.firebirdsql.gds.BlrConstants.blr_concatenate;
import static org.firebirdsql.gds.BlrConstants.blr_end;
import static org.firebirdsql.gds.BlrConstants.blr_eoc;
import static org.firebirdsql.gds.BlrConstants.blr_field;
import static org.firebirdsql.gds.BlrConstants.blr_literal;
import static org.firebirdsql.gds.BlrConstants.blr_message;
import static org.firebirdsql.gds.BlrConstants.blr_short;
import static org.firebirdsql.gds.BlrConstants.blr_text;
import static org.firebirdsql.gds.BlrConstants.blr_text2;
import static org.firebirdsql.gds.BlrConstants.blr_varying;
import static org.firebirdsql.gds.BlrConstants.blr_varying2;
import static org.firebirdsql.gds.BlrConstants.blr_version5;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;

import org.firebirdsql.gds.BlrConstants;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbStatement;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.gds.ng.wire.DefaultBlrCalculator;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class Blr {

    private byte[] b;
    private int p;

    private Blr(byte[] bytes) {
        this.b = bytes;
    }

    private int u8() {
        return b[p++] & 0xff;
    }

    private int u16() {
        int v = (b[p] & 0xff) | (b[p + 1] & 0xff) << 8;
        p += 2;
        return v;
    }

    private String str(int len) {
        String s = new String(b, p, len, StandardCharsets.ISO_8859_1);
        p += len;
        return s;
    }

    private static byte[] fetchBlob(Connection con, String sql) throws SQLException {
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(sql)) {
            if (!rs.next()) {
                throw new SQLException("no row");
            }
            return rs.getBytes(1);
        }
    }

    private static void hexDump(byte[] b, int limit) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < b.length && i < limit; i++) {
            sb.append(String.format("%02x", b[i] & 0xff)).append(i % 16 == 15 ? "\n" : " ");
        }
        sb.append(b.length > limit ? "... (" : "(").append(b.length).append(" bytes total)");
        System.out.println(sb);
    }

    /** Decode one expression - enough opcodes for a computed column. */
    private void expr(int depth) {
        String indent = "   ".repeat(depth);
        int op = u8();
        if (op == blr_concatenate) {
            System.out.println(indent + "blr_concatenate");
            expr(depth + 1);
            expr(depth + 1);
        } else if (op == blr_field) {
            int ctx = u8();
            int len = u8();
            System.out.println(indent + "blr_field context " + ctx + ", '" + str(len) + "'");
        } else if (op == blr_literal && (b[p] & 0xff) == blr_text2) {
            p++;
            int cs = u16();
            int len = u16();
            System.out.println(indent + "blr_literal blr_text2 charset " + cs + ", len " + len
                    + ", \"" + str(len) + "\"");
        } else {
            System.out.println(indent + "opcode " + op + " (decoder stops here)");
            p = b.length;
        }
    }

    /** Decode blr_message declarations, as found after blr_begin. */
    private void messages() {
        while (p < b.length && (b[p] & 0xff) == blr_message) {
            p++;
            int msg = u8();
            int count = u16();
            StringBuilder sb = new StringBuilder("blr_message " + msg + ", " + count + " fields:");
            for (int i = 0; i < count; i++) {
                int t = u8();
                if (t == blr_short) {
                    sb.append(" blr_short(scale ").append(u8()).append(')');
                } else if (t == blr_text2 || t == blr_varying2) {
                    sb.append(t == blr_text2 ? " blr_text2" : " blr_varying2")
                            .append("(cs ").append(u16()).append(", len ").append(u16()).append(')');
                } else if (t == blr_text || t == blr_varying) {
                    sb.append(t == blr_text ? " blr_text" : " blr_varying")
                            .append("(len ").append(u16()).append(')');
                } else {
                    sb.append(" dtype ").append(t).append('?');
                    i = count;
                }
            }
            System.out.println(sb);
        }
    }

    private static int opcodeCount() {
        int n = 0;
        for (Field f : BlrConstants.class.getFields()) {
            if (Modifier.isStatic(f.getModifiers()) && f.getName().startsWith("blr_")) {
                n++;
            }
        }
        return n;
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            System.out.println("(opcodes: " + opcodeCount()
                    + " blr_* constants in Jaybird's org.firebirdsql.gds.BlrConstants)");
            try (Connection con = FbSample.employee()) {
                System.out.println();
                System.out.println("== computed column EMPLOYEE.FULL_NAME - RDB$FIELDS.RDB$COMPUTED_BLR");
                Blr d = new Blr(fetchBlob(con,
                        "select f.rdb$computed_blr from rdb$fields f"
                        + " join rdb$relation_fields rf on f.rdb$field_name = rf.rdb$field_source"
                        + " where rf.rdb$relation_name = 'EMPLOYEE' and rf.rdb$field_name = 'FULL_NAME'"));
                System.out.println("(java type: " + d.b.getClass().getSimpleName() + ")");
                hexDump(d.b, 64);
                System.out.println(d.u8() == blr_version5 ? "blr_version5" : "unexpected version!");
                d.expr(1);
                System.out.println(d.p < d.b.length && (d.b[d.p] & 0xff) == blr_eoc ? "blr_eoc" : "(no blr_eoc?)");

                System.out.println();
                System.out.println("== procedure GET_EMP_PROJ - RDB$PROCEDURES.RDB$PROCEDURE_BLR");
                d = new Blr(fetchBlob(con, "select rdb$procedure_blr from rdb$procedures"
                        + " where rdb$procedure_name = 'GET_EMP_PROJ'"));
                hexDump(d.b, 32);
                System.out.print(d.u8() == blr_version5 ? "blr_version5, " : "?, ");
                System.out.println(d.u8() == blr_begin ? "blr_begin" : "?");
                d.messages();
                System.out.println("... " + (d.b.length - d.p)
                        + " more bytes - see isql SET BLOB ALL for the full dump");

                // -- the other direction: the BLR this driver writes ----------
                String sql = "select proj_id from employee_project where emp_no = ? and proj_id = ?";
                System.out.println();
                System.out.println("== BLR Jaybird writes for the parameters of");
                System.out.println("   " + sql);
                FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
                FbTransaction tx = db.startTransaction("set transaction read only");
                try {
                    FbStatement st = db.createStatement(tx);
                    try {
                        st.prepare(sql);
                        d = new Blr(DefaultBlrCalculator.CALCULATOR_DIALECT_3
                                .calculateBlr(st.getParameterDescriptor()));
                    } finally {
                        st.close();
                    }
                } finally {
                    tx.commit();
                }
                hexDump(d.b, 64);
                System.out.print(d.u8() == blr_version5 ? "blr_version5, " : "?, ");
                System.out.println(d.u8() == blr_begin ? "blr_begin" : "?");
                d.messages();
                System.out.print(d.u8() == blr_end ? "blr_end, " : "?, ");
                System.out.println(d.u8() == blr_eoc ? "blr_eoc" : "?");
            }
        });
    }
}
