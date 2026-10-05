//
// Intl.java - charset, collation and transliteration (Java twin of
// ../../../../../cpp/intl.cpp; see ../../../../../../internationalization.md).
//
// One table, three concepts: two UTF8 columns that differ only in collation
// (UNICODE_CI_AI vs UCS_BASIC) and a WIN1252 column beside them.  The
// Cafe/CAFE/cafe experiment shows the collation deciding equality and
// order; then the same stored WIN1252 'Cafe' (with e-acute) is fetched over
// connections with different connection charsets and hex-dumped.
//
// Jaybird keeps the two halves of "charset" apart that the Rust, Python and
// Go drivers fold into one setting: the `encoding` property is the
// Firebird connection charset (isc_dpb_lc_ctype), while Java's decoding is
// chosen per column from the charset id the server describes it with.  So
// ResultSet.getBytes() shows the wire bytes and getString() the decoded
// text, and on an encoding=NONE connection - where the server describes the
// column as WIN1252 and passes its raw byte through - Jaybird decodes that
// E9 with Java's windows-1252 codec: the transliteration still happens,
// only on the client side of the wire.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Intl
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.Properties;

public final class Intl {

    private static final String PICK = "select name_win from t where name_bin starting with 'Caf' "
            + "and name_bin <> 'CAFE' and name_bin <> 'cafe'";   // ASCII-only predicate

    private static String column(Connection con, String sql) throws SQLException {
        StringBuilder sb = new StringBuilder();
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(sql)) {
            while (rs.next()) {
                sb.append(rs.getString(1)).append("  ");
            }
        }
        return sb.toString();
    }

    /** Attach with a given Firebird connection charset and no Java charSet override. */
    private static Connection withEncoding(String path, String encoding) throws SQLException {
        Properties p = FbSample.props();
        p.remove("charSet");
        p.setProperty("encoding", encoding);
        return DriverManager.getConnection(FbSample.url(path), p);
    }

    private static void dump(String label, Connection con) throws SQLException {
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(PICK)) {
            rs.next();
            byte[] wire = rs.getBytes(1);
            StringBuilder hex = new StringBuilder();
            for (byte b : wire) {
                hex.append(String.format("%02X ", b));
            }
            System.out.printf("  %-18s len=%2d  %-15s getString() = \"%s\"%n",
                    label, wire.length, hex, rs.getString(1));
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("intl", args);
            try (Connection con = attachOrCreate(path)) {     // encoding UTF8 (charSet=UTF-8)
                execute(con, "recreate table t ("
                        + "  name_ci_ai varchar(30) character set utf8 collate unicode_ci_ai,"
                        + "  name_bin   varchar(30) character set utf8 collate ucs_basic,"
                        + "  name_win   varchar(30) character set win1252)");
                try (PreparedStatement ps = con.prepareStatement("insert into t values (?, ?, ?)")) {
                    for (String v : new String[] {"Café", "CAFE", "cafe"}) {
                        ps.setString(1, v);
                        ps.setString(2, v);
                        ps.setString(3, v);
                        ps.executeUpdate();
                    }
                }

                // -- 1. The collation, not the data, decides what "equal" means.
                System.out.println("rows matching 'cafe' with UNICODE_CI_AI : "
                        + scalar(con, "select count(*) from t where name_ci_ai = 'cafe'"));
                System.out.println("rows matching 'cafe' with UCS_BASIC     : "
                        + scalar(con, "select count(*) from t where name_bin = 'cafe'"));
                System.out.println("UPPER('café èñ ß')                      : "
                        + scalar(con, "select upper('café èñ ß') from rdb$database"));
                System.out.println("ORDER BY name_ci_ai: " + column(con, "select name_ci_ai from t order by name_ci_ai").strip());
                System.out.println("ORDER BY name_bin  : " + column(con, "select name_bin from t order by name_bin")
                        + "  (binary: uppercase codepoints first)");
                System.out.println();

                // -- 2./3. Same stored WIN1252 value, three connection charsets.
                System.out.println("SELECT name_win ... 'Café' - same row, three connections:");
                dump("encoding=UTF8:", con);
            }
            try (Connection none = withEncoding(path, "NONE")) {
                dump("encoding=NONE:", none);
            }
            try (Connection win = withEncoding(path, "WIN1252")) {
                dump("encoding=WIN1252:", win);
            }
            System.out.println("  -> the column stores E9 (WIN1252).  UTF8: the server transliterated to C3 A9.");
            System.out.println("     NONE and WIN1252: E9 crossed the wire and Jaybird decoded it with the");
            System.out.println("     column's own charset (windows-1252) into a Java String.");
            System.out.println("done.");
        });
    }
}
