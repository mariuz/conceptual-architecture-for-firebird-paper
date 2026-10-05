//
// Ha.java - the client-side HA primitives: a database SHADOW and the
// replica-mode switch (Java twin of ../../../../../cpp/ha.cpp; see
// ../../../../../../high-availability.md).
//
// As in the C++ sample, a shadow is created on a fresh scratch database,
// shown in RDB$FILES, measured (Files.size - server and sample share a
// host here) while 5000 rows go in, and retired with DROP SHADOW ...
// DELETE FILE.  CREATE/DROP SHADOW are ordinary DSQL, so Jaybird's pure-Java
// wire protocol needs nothing special.  The instructive addition is the
// second primitive the C++ sample leaves as text: replica promotion.  The
// gfix -replica switch is just a DPB item (isc_dpb_set_db_replica), which
// Jaybird exposes as the connection property set_db_replica - so the sample
// marks the database a read-only replica, proves a write is refused, and
// promotes it back with set_db_replica=0 (gfix -replica none), reading the
// mode from MON$DATABASE each time.  No replication.conf is involved: this
// is the mode flag on the header page, not log shipping.  (The shadow's
// recovery verbs, gfix -activate / -kill, are
// FBMaintenanceManager.activateShadowFile / killUnavailableShadows; showing
// them needs a lost main file.)
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Ha
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.recreate;
import static fbsamples.FbSample.scalar;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;

public final class Ha {

    private static long size(String p) {
        try {
            return Files.size(Path.of(p));
        } catch (IOException e) {
            return -1;
        }
    }

    private static void showFiles(String when, String main, String shadow) {
        System.out.printf("%-28s main = %8d bytes, shadow = %8d bytes%n", when, size(main), size(shadow));
    }

    private static String replicaMode(Connection con) throws SQLException {
        Object m = scalar(con, "select mon$replica_mode from mon$database");
        return switch (((Number) m).intValue()) {
            case 0 -> "0 (NONE - a primary)";
            case 1 -> "1 (READ_ONLY replica)";
            case 2 -> "2 (READ_WRITE replica)";
            default -> m.toString();
        };
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String main = dbPath("ha", args);
            String shadow = main.replaceAll("\\.fdb$", "") + ".shd";
            try (Connection con = recreate(main)) {
                execute(con, "CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))");

                // 1. Create the synchronous page-level mirror.
                execute(con, "CREATE SHADOW 1 '" + shadow + "'");
                System.out.println("CREATE SHADOW 1 done - the engine dumped every page to the mirror");
                try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(
                        "SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS "
                        + "FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER")) {
                    while (rs.next()) {
                        System.out.printf("RDB$FILES: %s  shadow_number=%d  flags=%d%n",
                                rs.getString(1).strip(), rs.getInt(2), rs.getInt(3));
                    }
                }
                showFiles("after CREATE SHADOW:", main, shadow);

                // 2. Write load: every page write now goes to both files.
                execute(con, "EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
                        + "  WHILE (I < 5000) DO BEGIN "
                        + "    INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1; "
                        + "  END "
                        + "END");
                showFiles("after 5000 inserts:", main, shadow);

                // 3. Retire the mirror.
                execute(con, "DROP SHADOW 1 DELETE FILE");
                System.out.println("DROP SHADOW 1 DELETE FILE done");
                showFiles("after DROP SHADOW:", main, shadow);
                System.out.println("RDB$FILES rows left: " + scalar(con, "SELECT COUNT(*) FROM RDB$FILES"));
            }

            // 4. Replica mode is a DPB item: demote to a read-only replica...
            System.out.println();
            try (Connection con = attach(main, "set_db_replica=1")) {
                System.out.println("attached with set_db_replica=1:  replica mode = " + replicaMode(con));
                try {
                    execute(con, "INSERT INTO HA_LOG VALUES (-1, 'write on a replica')");
                    System.out.println("unexpected: the replica accepted a write");
                } catch (SQLException e) {
                    System.out.println("  user write refused: " + e.getMessage());
                }
            }
            // ...and promote it back to a primary (gfix -replica none).
            try (Connection con = attach(main, "set_db_replica=0")) {
                System.out.println("attached with set_db_replica=0:  replica mode = " + replicaMode(con));
                execute(con, "INSERT INTO HA_LOG VALUES (-1, 'write after promotion')");
                System.out.println("  write after promotion OK, rows = " + scalar(con, "SELECT COUNT(*) FROM HA_LOG"));
            }
            System.out.println("done.");
        });
    }
}
