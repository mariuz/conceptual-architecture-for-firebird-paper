//
// Backup.java - a gbak backup + restore round trip through the Services API
// (Java twin of ../../../../../cpp/backup.cpp; see
// ../../../../../../backup-and-recovery.md).
//
//   1. create a scratch database with a table and three rows;
//   2. FBBackupManager: isc_action_svc_backup (verbose) to a server-side
//      .fbk while the source attachment stays open (gbak reads through a
//      snapshot - "online backup");
//   3. FBBackupManager: isc_action_svc_restore (replace), verbose;
//   4. attach to the restored copy and prove the rows survived;
//   5. what only some drivers offer: FBStreamingBackupManager runs the same
//      service with the backup file set to "stdout", so the .fbk bytes
//      travel over the service connection INTO THE JVM (here a byte array,
//      in practice a local file or a socket), and a restore streams them
//      back - the server never writes a backup file of its own.
//
// Jaybird reimplements the Services API in Java (like the Go driver, no
// gbak binary and no libfbclient): the managers build the SPBs from setters
// and run the isc_info_svc_line drain loop, writing gbak's log to the
// OutputStream given to setLogger().
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Backup
//
package fbsamples;

import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.SQLException;

import org.firebirdsql.management.FBBackupManager;
import org.firebirdsql.management.FBServiceManager;
import org.firebirdsql.management.FBStreamingBackupManager;

public final class Backup {

    /** gbak's log, one prefixed line at a time. */
    private static final class Prefixed extends OutputStream {
        private final ByteArrayOutputStream line = new ByteArrayOutputStream();

        @Override
        public void write(int b) {
            if (b == '\n') {
                flush();
            } else {
                line.write(b);
            }
        }

        @Override
        public void flush() {
            if (line.size() > 0) {
                System.out.println("  gbak> " + line.toString(StandardCharsets.UTF_8));
                line.reset();
            }
        }
    }

    private static <T extends FBServiceManager> T service(T m) {
        m.setServerName(FbSample.HOST);
        m.setUser(FbSample.USER);
        m.setPassword(FbSample.PASSWORD);
        return m;
    }

    private static void verify(String label, String path) throws SQLException {
        try (Connection con = FbSample.attach(path)) {
            System.out.println(label + " says: " + scalar(con, "select count(*) from br_items")
                    + " rows, max name = " + scalar(con, "select max(name) from br_items"));
        }
    }

    public static void main(String[] args) {
        String src = dbPath("backup", args);
        String fbk = src.replaceFirst("\\.fdb$", "") + ".fbk";
        String restored = src.replaceFirst("\\.fdb$", "") + "_restored.fdb";
        String streamed = src.replaceFirst("\\.fdb$", "") + "_streamed.fdb";
        FbSample.run(() -> {
            // -- 1. scratch source database, kept open during the backup -------
            try (Connection con = FbSample.recreate(src)) {
                execute(con, "create table br_items (id int not null primary key, name varchar(30))");
                execute(con, "insert into br_items values (1, 'alpha')");
                execute(con, "insert into br_items values (2, 'beta')");
                execute(con, "insert into br_items values (3, 'gamma')");
                System.out.println("source ready: BR_ITEMS with 3 rows");

                // -- 2. backup through the service, verbose -----------------------
                Prefixed log = new Prefixed();
                System.out.println();
                System.out.println("== backup: " + src + " -> " + fbk + " ==");
                FBBackupManager backup = service(new FBBackupManager());
                backup.setDatabase(src);
                backup.setBackupPath(fbk);
                backup.setVerbose(true);
                backup.setLogger(log);
                backup.backupDatabase();
                log.flush();

                // -- 3. restore (replace) through the service ---------------------
                System.out.println();
                System.out.println("== restore: " + fbk + " -> " + restored + " ==");
                FBBackupManager restore = service(new FBBackupManager());
                restore.setBackupPath(fbk);
                restore.setDatabase(restored);
                restore.setRestoreReplace(true);
                restore.setVerbose(true);
                restore.setLogger(log);
                restore.restoreDatabase();
                log.flush();
            }

            // -- 4. the restored copy has the data ---------------------------------
            System.out.println();
            verify("restored database", restored);

            // -- 5. streaming: the .fbk crosses the wire into the JVM ------------
            System.out.println();
            System.out.println("== streaming backup: the server sends the .fbk to us ==");
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            FBStreamingBackupManager sb = service(new FBStreamingBackupManager());
            sb.setDatabase(src);
            sb.setBackupOutputStream(bytes);
            sb.backupDatabase();
            byte[] fbkBytes = bytes.toByteArray();
            System.out.println("received " + fbkBytes.length + " bytes of backup into a Java byte[]");

            FBStreamingBackupManager sr = service(new FBStreamingBackupManager());
            sr.setDatabase(streamed);
            sr.setRestoreInputStream(new ByteArrayInputStream(fbkBytes));
            sr.setRestoreReplace(true);
            sr.restoreDatabase();
            verify("restored from the stream, " + streamed, streamed);
            System.out.println("done.");
        });
    }
}
