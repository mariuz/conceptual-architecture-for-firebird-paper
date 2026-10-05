//
// Extensibility.java - calling native code through SQL: UDR end to end
// (Java twin of ../../../../../cpp/extensibility.cpp; see
// ../../../../../../extensibility.md).
//
// The shipped example UDR module (plugins/udr/libudrcpp_example.so) is bound
// to SQL names with EXTERNAL NAME '<module>!<entry>' ENGINE udr and called
// like any other procedure/function; then RDB$PROCEDURES / RDB$FUNCTIONS
// show the binding as metadata and RDB$CONFIG names the plugin filling each
// role.  Every seam lives on the server, so Jaybird's pure-Java wire
// protocol loses nothing.  The JDBC idiom adds two things: the calls are
// PreparedStatements with ? parameters (the C++ sample inlines literals),
// and DatabaseMetaData.getProcedures / getFunctions - the driver's
// portable catalog API - reports the external routines next to the raw
// RDB$ query, with the module!entry binding nowhere in sight.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Extensibility
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.text;

import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

public final class Extensibility {

    /** isql-style listing of a result set: headers from ResultSetMetaData. */
    static void table(ResultSet rs) throws SQLException {
        ResultSetMetaData md = rs.getMetaData();
        int n = md.getColumnCount();
        List<String[]> rows = new ArrayList<>();
        int[] w = new int[n];
        String[] names = new String[n];
        for (int i = 0; i < n; i++) {
            names[i] = md.getColumnLabel(i + 1);
            w[i] = names[i].length();
        }
        while (rs.next()) {
            String[] r = new String[n];
            for (int i = 0; i < n; i++) {
                Object v = rs.getObject(i + 1);
                r[i] = v == null ? "" : text(v).strip();
                w[i] = Math.max(w[i], r[i].length());
            }
            rows.add(r);
        }
        System.out.println(line(names, w));
        String[] dashes = new String[n];
        for (int i = 0; i < n; i++) {
            dashes[i] = "-".repeat(w[i]);
        }
        System.out.println(line(dashes, w));
        for (String[] r : rows) {
            System.out.println(line(r, w));
        }
    }

    private static String line(String[] cells, int[] w) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < cells.length; i++) {
            sb.append(i == 0 ? "" : " ").append(String.format("%-" + w[i] + "s", cells[i]));
        }
        return sb.toString().stripTrailing();
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            try (Connection con = attachOrCreate(dbPath("extensibility", args))) {
                // 1. Bind SQL names to entry points in the shipped native module.
                execute(con, "recreate procedure gen_rows (start_n integer not null, "
                        + "                             end_n integer not null) "
                        + "  returns (n integer not null) "
                        + "  external name 'udrcpp_example!gen_rows' engine udr");
                execute(con, "recreate function sum_args (n1 integer, n2 integer, n3 integer) "
                        + "  returns integer "
                        + "  external name 'udrcpp_example!sum_args' engine udr");

                // 2. Call them: native C++ running inside the server, ? parameters here.
                System.out.println("select n from gen_rows(?, ?)  [1, 5]:");
                try (PreparedStatement ps = con.prepareStatement("select n from gen_rows(?, ?)")) {
                    ps.setInt(1, 1);
                    ps.setInt(2, 5);
                    try (ResultSet rs = ps.executeQuery()) {
                        table(rs);
                    }
                }
                try (PreparedStatement ps = con.prepareStatement(
                        "select sum_args(?, ?, ?) from rdb$database")) {
                    ps.setInt(1, 19);
                    ps.setInt(2, 20);
                    ps.setInt(3, 3);
                    try (ResultSet rs = ps.executeQuery()) {
                        rs.next();
                        System.out.println("\nselect sum_args(19, 20, 3):  " + rs.getInt(1));
                    }
                }

                // 3. The binding is ordinary metadata...
                System.out.println("\nexternal routines recorded in the system tables:");
                try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(
                        "select trim(rdb$procedure_name) || '  ->  ' || "
                        + "       trim(rdb$entrypoint) || '  (engine ' || "
                        + "       trim(rdb$engine_name) || ')' "
                        + "from rdb$procedures where rdb$engine_name = 'UDR' "
                        + "union all "
                        + "select trim(rdb$function_name) || '  ->  ' || "
                        + "       trim(rdb$entrypoint) || '  (engine ' || "
                        + "       trim(rdb$engine_name) || ')' "
                        + "from rdb$functions where rdb$engine_name = 'UDR'")) {
                    while (rs.next()) {
                        System.out.println(rs.getString(1));
                    }
                }

                // ...which JDBC's portable catalog API flattens away.
                DatabaseMetaData dmd = con.getMetaData();
                System.out.println("\nthe same routines via DatabaseMetaData:");
                try (ResultSet rs = dmd.getProcedures(null, null, "GEN_ROWS")) {
                    while (rs.next()) {
                        System.out.println("getProcedures: " + rs.getString("PROCEDURE_NAME")
                                + (rs.getShort("PROCEDURE_TYPE") == DatabaseMetaData.procedureReturnsResult
                                ? "  procedureReturnsResult" : "  type " + rs.getShort("PROCEDURE_TYPE")));
                    }
                }
                try (ResultSet rs = dmd.getFunctions(null, null, "SUM_ARGS")) {
                    while (rs.next()) {
                        System.out.println("getFunctions:  " + rs.getString("FUNCTION_NAME")
                                + (rs.getShort("FUNCTION_TYPE") == DatabaseMetaData.functionNoTable
                                ? "  functionNoTable" : "  type " + rs.getShort("FUNCTION_TYPE")));
                    }
                }

                // 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG.
                System.out.println("\nplugins filling each role (rdb$config):");
                try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(
                        "select rdb$config_name, rdb$config_value "
                        + "from rdb$config "
                        + "where rdb$config_name in ('Providers', 'AuthServer', "
                        + "      'UserManager', 'WireCryptPlugin', 'TracePlugin', "
                        + "      'DefaultProfilerPlugin') "
                        + "order by rdb$config_id")) {
                    table(rs);
                }
            }
            System.out.println("\ndone.");
        });
    }
}
