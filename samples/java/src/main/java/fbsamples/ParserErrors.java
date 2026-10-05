//
// ParserErrors.java - driving Firebird's SQL parser from the client (Java
// twin of ../../../../../cpp/parser_errors.cpp; see
// ../../../../../../grammar-and-parser.md).
//
// Six strings are prepared against the stock employee database: a `?`
// placeholder that comes back as a typed input parameter, FIRST in its two
// grammatical roles (row-limit clause and plain column name), two syntax
// errors with token line/column, and a semantic error whose position
// survives the parse.  Jaybird's prepareStatement is a genuine prepare-only
// step (op_prepare_statement goes out at once, nothing executes), and unlike
// the other pure wire drivers it publishes what the prepare response
// describes: FirebirdPreparedStatement.getStatementType() is the
// isc_info_sql_stmt_* code, ParameterMetaData the `?` descriptor.  A failed
// prepare is one flattened SQLException, but its cause is an
// FBSQLExceptionInfo chain holding each status-vector item with its own gds
// code - so the sample tells isc_dsql_token_unk_err (syntax) from
// isc_dsql_field_err (semantic) programmatically.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=ParserErrors
//
package fbsamples;

import java.sql.Connection;
import java.sql.ParameterMetaData;
import java.sql.PreparedStatement;
import java.sql.SQLException;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.jdbc.FirebirdPreparedStatement;

public final class ParserErrors {

    private static String typeName(int t) {
        return switch (t) {
            case FirebirdPreparedStatement.TYPE_SELECT -> "SELECT";
            case FirebirdPreparedStatement.TYPE_INSERT -> "INSERT";
            case FirebirdPreparedStatement.TYPE_UPDATE -> "UPDATE";
            case FirebirdPreparedStatement.TYPE_DDL -> "DDL";
            default -> "other";
        };
    }

    /** Feed one string to the parser; report the statement's shape or the status vector. */
    private static void tryPrepare(Connection con, String sql) {
        System.out.println("---- " + sql);
        try (PreparedStatement ps = con.prepareStatement(sql)) {
            FirebirdPreparedStatement fps = ps.unwrap(FirebirdPreparedStatement.class);
            ParameterMetaData in = ps.getParameterMetaData();
            System.out.printf("  parsed OK: type=%s, input params=%d, output columns=%d%n",
                    typeName(fps.getStatementType()), in.getParameterCount(),
                    ps.getMetaData().getColumnCount());
            for (int i = 1; i <= in.getParameterCount(); i++) {
                System.out.printf("    param %d: %s (java.sql.Types %d), precision=%d%n", i - 1,
                        in.getParameterTypeName(i), in.getParameterType(i), in.getPrecision(i));
            }
        } catch (SQLException e) {
            // The cause chain holds one FBSQLExceptionInfo per status-vector item.
            String kind = "?";
            StringBuilder codes = new StringBuilder();
            if (e.getCause() instanceof SQLException info) {
                for (SQLException x = info; x != null; x = x.getNextException()) {
                    codes.append(codes.isEmpty() ? "" : " ").append(x.getErrorCode());
                    if (x.getErrorCode() == ISCConstants.isc_dsql_token_unk_err) {
                        kind = "syntax";
                    } else if (x.getErrorCode() == ISCConstants.isc_dsql_field_err) {
                        kind = "semantic";
                    }
                }
            }
            System.out.println("  prepare failed (" + kind + "; SQLState " + e.getSQLState()
                    + ", gds " + e.getErrorCode() + "):");
            System.out.println(e.getMessage());
            System.out.println("  status-vector gds codes: " + codes);
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            try (Connection con = FbSample.employee()) {
                con.setAutoCommit(false);

                // 1. Dynamic SQL: the `?` becomes a typed parameter.
                tryPrepare(con, "SELECT first_name FROM employee WHERE emp_no = ?");

                // 2. One token, two grammatical roles: FIRST as row-limit clause...
                tryPrepare(con, "SELECT FIRST 1 emp_no FROM employee");
                // ...and FIRST as an ordinary identifier (non-reserved keyword).
                tryPrepare(con, "SELECT first FROM (SELECT 1 AS first FROM rdb$database)");

                // 3. Syntax errors with token position.
                tryPrepare(con, "SELEC 1 FROM rdb$database");
                tryPrepare(con, "SELECT emp_no\nFROM employee\nWHERE ORDER BY 1");

                // 4. Semantic error - still carries line/column.
                tryPrepare(con, "SELECT frst_name\nFROM employee");

                con.commit();
            }
            System.out.println("done.");
        });
    }
}
