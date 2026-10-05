//
// FbSample.java - shared boilerplate for the Java hands-on twins.
//
// Every sample demonstrates one companion document of the paper.  They use
// Jaybird (https://github.com/FirebirdSQL/jaybird), the FirebirdSQL
// project's JDBC driver.  Its default PURE_JAVA protocol is an independent
// Java implementation of the wire protocol (no libfbclient, like
// node-firebird and the Go driver); the NATIVE and EMBEDDED protocols of
// jaybird-native load libfbclient or the engine in-process through JNA.
// Beyond JDBC it exposes Firebird's own layer: TPB building
// (FirebirdConnection.setTransactionParameters), the Services API
// (org.firebirdsql.management: backup, statistics, maintenance, trace,
// users), events, and the wire-level GDS API (org.firebirdsql.gds.ng).
//
// Like the other twins, everything runs against the local server with
// scratch databases under /tmp/fbhandson (SYSDBA/masterkey, overridable via
// ISC_USER / ISC_PASSWORD; FB_HOST picks another server).
//
package fbsamples;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

public final class FbSample {

    public static final String SCRATCH = "/tmp/fbhandson";
    public static final String HOST = env("FB_HOST", "localhost");
    public static final String USER = env("ISC_USER", "SYSDBA");
    public static final String PASSWORD = env("ISC_PASSWORD", "masterkey");

    private FbSample() {
    }

    private static String env(String name, String def) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? def : v;
    }

    /** Server path of a topic's scratch database; args[0] overrides it. */
    public static String dbPath(String topic, String[] args) {
        return args.length > 0 ? args[0] : SCRATCH + "/" + topic + "_java.fdb";
    }

    /** JDBC URL of a server path or alias over the (pure Java) wire protocol. */
    public static String url(String path) {
        return "jdbc:firebird://" + HOST + "/" + path;
    }

    /** Connection properties: credentials, UTF8, plus extra key=value pairs. */
    public static Properties props(String... extra) {
        Properties p = new Properties();
        p.setProperty("user", USER);
        p.setProperty("password", PASSWORD);
        p.setProperty("charSet", "UTF-8");
        for (String kv : extra) {
            int eq = kv.indexOf('=');
            p.setProperty(kv.substring(0, eq), kv.substring(eq + 1));
        }
        return p;
    }

    /** Attach to an existing database. */
    public static Connection attach(String path, String... extra) throws SQLException {
        return DriverManager.getConnection(url(path), props(extra));
    }

    /** Attach, creating the database first if it does not exist. */
    public static Connection attachOrCreate(String path, String... extra) throws SQLException {
        String[] all = new String[extra.length + 1];
        System.arraycopy(extra, 0, all, 0, extra.length);
        all[extra.length] = "createDatabaseIfNotExist=true";
        return DriverManager.getConnection(url(path), props(all));
    }

    /** A fresh scratch database: drop it if it exists, then create it. */
    public static Connection recreate(String path, String... extra) throws SQLException {
        try (Connection old = attach(path, extra)) {
            old.unwrap(org.firebirdsql.jdbc.FirebirdConnection.class)
                    .getFbDatabase().dropDatabase();
        } catch (SQLException ignored) {
            // did not exist
        }
        return attachOrCreate(path, extra);
    }

    /** The demo server's employee database (charset NONE, like its data). */
    public static Connection employee(String... extra) throws SQLException {
        Properties p = props(extra);
        if (!p.containsKey("charSet") || extra.length == 0) {
            p.remove("charSet");
            p.setProperty("encoding", "NONE");
        }
        return DriverManager.getConnection(url(env("FB_DATABASE", "employee")), p);
    }

    /** Run a statement (auto-commit applies when the connection has it on). */
    public static void execute(Connection con, String sql) throws SQLException {
        try (Statement st = con.createStatement()) {
            st.execute(sql);
        }
    }

    /** The first column of the first row, or null. */
    public static Object scalar(Connection con, String sql) throws SQLException {
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(sql)) {
            return rs.next() ? rs.getObject(1) : null;
        }
    }

    /** Every row of a query as Object[]. */
    public static List<Object[]> rows(Connection con, String sql) throws SQLException {
        List<Object[]> out = new ArrayList<>();
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(sql)) {
            ResultSetMetaData md = rs.getMetaData();
            int n = md.getColumnCount();
            while (rs.next()) {
                Object[] row = new Object[n];
                for (int i = 0; i < n; i++) {
                    row[i] = rs.getObject(i + 1);
                }
                out.add(row);
            }
        }
        return out;
    }

    /** A value rendered for display, NULL as <null>. */
    public static String text(Object v) {
        if (v == null) {
            return "<null>";
        }
        if (v instanceof byte[] b) {
            return new String(b);
        }
        return v.toString();
    }

    /** An SQLException's message plus every chained exception, one per line. */
    public static String errorText(SQLException e) {
        StringBuilder sb = new StringBuilder(e.getMessage());
        for (SQLException next = e.getNextException(); next != null; next = next.getNextException()) {
            sb.append('\n').append(next.getMessage());
        }
        return sb.toString();
    }

    /** A sample's body. */
    @FunctionalInterface
    public interface Body {
        void run() throws Exception;
    }

    /** Run a sample, exiting 1 with the engine's message on error. */
    public static void run(Body body) {
        try {
            body.run();
        } catch (SQLException e) {
            System.err.println("error: " + errorText(e));
            System.exit(1);
        } catch (Exception e) {
            e.printStackTrace();
            System.exit(1);
        }
    }
}
