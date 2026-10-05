//
// Catalog.java - the catalog describing itself, from client SQL (Java twin
// of ../../../../../cpp/catalog.cpp; see ../../../../../../catalog-bootstrap.md).
//
// On a freshly recreated database: (1) the fixed relation ids of
// relations.h (RDB$PAGES 0, RDB$DATABASE 1, RDB$FIELDS 2, RDB$RELATIONS 6);
// (2) RDB$PAGES carrying its own pointer page, cross-checked against the
// hdr_PAGES word at byte 28 of page 0 - read with a FileChannel and a
// little-endian ByteBuffer, below any driver; (3) RDB$FORMATS empty while
// the system relations are fully usable (their formats are compiled into
// the engine); (4) user DDL planting the first RDB$FORMATS rows.  The JDBC
// addition: DatabaseMetaData.getTables classifies the same relations into
// "SYSTEM TABLE" / "TABLE" from RDB$SYSTEM_FLAG - the portable catalog API
// sitting on top of the self-describing one.  Run it on the server machine.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Catalog
//
package fbsamples;

import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.recreate;
import static fbsamples.FbSample.scalar;
import static fbsamples.FbSample.text;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

public final class Catalog {

    /** isql-style listing; headers from ResultSetMetaData unless given. */
    private static void table(Connection con, String sql, String... headers) throws SQLException {
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(sql)) {
            ResultSetMetaData md = rs.getMetaData();
            int n = md.getColumnCount();
            List<String[]> rows = new ArrayList<>();
            String[] names = new String[n];
            int[] w = new int[n];
            for (int i = 0; i < n; i++) {
                names[i] = headers.length > 0 ? headers[i] : md.getColumnLabel(i + 1);
                w[i] = names[i].length();
            }
            while (rs.next()) {
                String[] r = new String[n];
                for (int i = 0; i < n; i++) {
                    r[i] = text(rs.getObject(i + 1)).strip();
                    w[i] = Math.max(w[i], r[i].length());
                }
                rows.add(r);
            }
            String[] dashes = new String[n];
            for (int i = 0; i < n; i++) {
                dashes[i] = "-".repeat(w[i]);
            }
            print(names, w);
            print(dashes, w);
            for (String[] r : rows) {
                print(r, w);
            }
        }
    }

    private static void print(String[] cells, int[] w) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < cells.length; i++) {
            sb.append(i == 0 ? "" : " ").append(String.format("%-" + w[i] + "s", cells[i]));
        }
        System.out.println(sb.toString().stripTrailing());
    }

    private static long hdrPages(String file) throws java.io.IOException {
        try (FileChannel ch = FileChannel.open(Path.of(file), StandardOpenOption.READ)) {
            ByteBuffer b = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
            ch.read(b, 28);
            return Integer.toUnsignedLong(b.getInt(0));
        }
    }

    private static int countTables(Connection con, String type) throws SQLException {
        int n = 0;
        try (ResultSet rs = con.getMetaData().getTables(null, null, "%", new String[] {type})) {
            while (rs.next()) {
                n++;
            }
        }
        return n;
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("catalog", args);
            try (Connection con = recreate(path)) {
                System.out.println("-- 1. fixed relation ids (relations.h declaration order) --");
                table(con, "select rdb$relation_id, trim(rdb$relation_name) "
                        + "from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1", "ID", "NAME");

                System.out.println("\n-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --");
                table(con, "select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type "
                        + "from rdb$pages where rdb$relation_id in (0, 6) "
                        + "order by rdb$relation_id, rdb$page_type, rdb$page_number");
                System.out.println("\nhdr_PAGES (page 0, offset 28) = " + hdrPages(path)
                        + "  <- matches the (relation 0, type 4) row above");

                System.out.println("\n-- 3. formats as code: zero stored formats, yet a full catalog --");
                table(con, "select (select count(*) from rdb$formats), "
                        + "       (select count(*) from rdb$relations where rdb$system_flag = 1), "
                        + "       (select count(*) from rdb$relation_fields r join rdb$relations rel "
                        + "          on r.rdb$relation_name = rel.rdb$relation_name "
                        + "          and r.rdb$schema_name = rel.rdb$schema_name "
                        + "        where rel.rdb$system_flag = 1) "
                        + "from rdb$database", "FORMATS_ROWS", "SYS_RELATIONS", "SYS_FIELDS");
                System.out.println("DatabaseMetaData.getTables: " + countTables(con, "SYSTEM TABLE")
                        + " SYSTEM TABLE, " + countTables(con, "TABLE") + " TABLE");

                System.out.println("\n-- 4. user DDL writes formats into the catalog --");
                execute(con, "create table t1 (a integer)");
                execute(con, "alter table t1 add b varchar(10)");
                table(con, "select rdb$relation_id, rdb$format, octet_length(rdb$descriptor) "
                        + "from rdb$formats order by rdb$relation_id, rdb$format",
                        "RDB$RELATION_ID", "RDB$FORMAT", "DESCRIPTOR_BYTES");
                System.out.println("\n(relation id of T1: "
                        + scalar(con, "select rdb$relation_id from rdb$relations where rdb$relation_name = 'T1'")
                        + " - the first user id; system tables still contribute no rows)");
                System.out.println("DatabaseMetaData.getTables: " + countTables(con, "SYSTEM TABLE")
                        + " SYSTEM TABLE, " + countTables(con, "TABLE") + " TABLE");
            }
            System.out.println("done.");
        });
    }
}
