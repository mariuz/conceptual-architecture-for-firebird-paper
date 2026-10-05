//
// Services.java - the Services API from Java: a service attachment, an
// information request, and a verbose server-side backup (Java twin of
// ../../../../../cpp/services.cpp; see ../../../../../../services-api.md).
//
// Jaybird speaks the Services protocol itself on its pure-Java wire
// (op_service_attach / op_service_start / op_service_info), so, as with
// the Go and JavaScript drivers, "localhost" can only mean the remote
// service_mgr - there is no embedded one to attach by mistake.  The
// org.firebirdsql.management classes wrap it: FBServiceManager.
// getServerVersion() is the isc_info_svc_server_version request, and
// FBBackupManager.backupDatabase() starts isc_action_svc_backup with SERVER
// paths and drains the output into the OutputStream given to setLogger().
// The instructive difference is the drain: Jaybird asks for
// isc_info_svc_to_eof, not isc_info_svc_line, with a 1 KB result buffer -
// each op_service_info round trip returns as much of gbak's output as fits,
// so the 74 verbose lines arrive in a handful of chunks instead of one poll
// per line.  The sample counts both.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Services
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;

import org.firebirdsql.management.FBBackupManager;
import org.firebirdsql.management.FBServiceManager;
import org.firebirdsql.management.ServiceManager;

public final class Services {

    /** Collects gbak's lines and counts the chunks (one per op_service_info answer with data). */
    private static final class Drain extends OutputStream {
        final List<String> lines = new ArrayList<>();
        final List<Integer> chunkSizes = new ArrayList<>();
        private final ByteArrayOutputStream line = new ByteArrayOutputStream();

        @Override
        public void write(int b) {
            if (b == '\n') {
                lines.add(line.toString(StandardCharsets.UTF_8).stripTrailing());
                line.reset();
            } else {
                line.write(b);
            }
        }

        @Override
        public void write(byte[] b, int off, int len) {
            chunkSizes.add(len);
            for (int i = off; i < off + len; i++) {
                write(b[i]);
            }
        }
    }

    private static <T extends ServiceManager> T configure(T m) {
        m.setServerName(FbSample.HOST);
        m.setUser(FbSample.USER);
        m.setPassword(FbSample.PASSWORD);
        return m;
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String db = dbPath("services", args);
            String fbk = args.length > 1 ? args[1] : db.replaceAll("\\.fdb$", "") + ".fbk";

            // 0. The scratch database (idempotent).
            try (Connection con = attachOrCreate(db)) {
                try {
                    execute(con, "create table t (id int, v varchar(20))");
                } catch (SQLException alreadyThere) {
                    // fine
                }
            }

            // 1+2. Service attachment + information request.
            FBServiceManager sm = configure(new FBServiceManager());
            System.out.println("service       : " + FbSample.HOST + ":service_mgr (pure-Java wire)");
            System.out.println("server version: " + sm.getServerVersion());

            // 3+4. The backup action, drained by Jaybird into our logger.
            FBBackupManager bm = configure(new FBBackupManager());
            bm.setDatabase(db);                      // server path!
            bm.setBackupPath(fbk);                   // server path!
            bm.setVerbose(true);
            Drain drain = new Drain();
            bm.setLogger(drain);
            System.out.println("backup started (verbose) - Jaybird drains with isc_info_svc_to_eof:");
            bm.backupDatabase();

            int n = drain.lines.size();
            for (int i = 0; i < n; i++) {
                if (i < 3 || i == n - 1) {
                    System.out.println("  " + drain.lines.get(i));
                } else if (i == 3) {
                    System.out.println("  ...");
                }
            }
            System.out.println("done: " + n + " gbak lines in " + drain.chunkSizes.size()
                    + " chunks of output (bytes per chunk: " + drain.chunkSizes + ")");

            Path file = Path.of(fbk);
            if (Files.isReadable(file.getParent())) {
                System.out.println("the file " + fbk + " now exists on the SERVER: " + Files.size(file)
                        + " bytes, owned by " + Files.getOwner(file).getName());
            }
        });
    }

    private Services() {
    }
}
