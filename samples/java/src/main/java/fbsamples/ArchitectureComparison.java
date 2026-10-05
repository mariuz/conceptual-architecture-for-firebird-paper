//
// ArchitectureComparison.java - one driver, three ways into the engine
// (Java twin of ../../../../../cpp/architecture_comparison.cpp; see
// ../../../../../../architecture-comparison.md).
//
// The C++ twin attaches twice through libfbclient: inet://localhost/employee
// goes to the Y-valve's Remote provider, a bare local path loads the Engine
// provider INTO the process.  Jaybird holds both driver families at once,
// chosen by the JDBC URL:
//
//   1. jdbc:firebird://localhost/employee   - PURE_JAVA: Jaybird's own
//      implementation of the wire protocol, no libfbclient at all;
//   2. jdbc:firebird:embedded:inet://localhost/employee - jaybird-native
//      loads libfbclient through JNA and hands it the string unchanged, so
//      the Y-valve's Remote provider does the talking, exactly as in the
//      C++ sample;
//   3. jdbc:firebird:embedded:/tmp/fbhandson/arch_embedded_java.fdb - the
//      same libfbclient, a local path, and the Y-valve loads the Engine
//      provider into the JVM.
//
// Legs 2 and 3 deliberately share ONE library handle (the EMBEDDED
// factory): jdbc:firebird:native:// would load libfbclient a second time,
// and with both loaded Jaybird 6.0.6's shutdown hook calls fb_shutdown on
// each and the JVM died with SIGSEGV at exit.
//
// Each attachment answers the C++ sample's three questions (ENGINE_VERSION,
// NETWORK_PROTOCOL, MON$SERVER_PID vs our own pid) plus the
// isc_info_firebird_version lines the driver keeps (GDSServerVersion
// .getRawVersions()): one line per layer the request crossed.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=ArchitectureComparison
//
package fbsamples;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.List;

import org.firebirdsql.gds.impl.GDSServerVersion;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class ArchitectureComparison {

    private static void inspect(String label, String url, boolean create) throws Exception {
        String[] extra = create ? new String[] {"createDatabaseIfNotExist=true"} : new String[0];
        java.util.Properties p = FbSample.props(extra);
        p.remove("charSet");
        p.setProperty("encoding", "NONE");
        try (Connection con = DriverManager.getConnection(url, p);
             Statement st = con.createStatement();
             ResultSet rs = st.executeQuery(
                     "select rdb$get_context('SYSTEM', 'ENGINE_VERSION'), "
                     + "       rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), "
                     + "       a.mon$server_pid "
                     + "from mon$attachments a "
                     + "where a.mon$attachment_id = current_connection")) {
            rs.next();
            String version = rs.getString(1);
            String protocol = rs.getString(2);
            long serverPid = rs.getLong(3);
            long pid = ProcessHandle.current().pid();

            GDSServerVersion sv = con.unwrap(FirebirdConnection.class).getFbDatabase().getServerVersion();
            List<String> raw = sv.getRawVersions();

            System.out.println(label);
            System.out.println("    JDBC URL          : " + url);
            System.out.println("    ENGINE_VERSION    : " + version);
            System.out.println("    NETWORK_PROTOCOL  : " + (protocol == null ? "<null>" : protocol));
            System.out.println("    MON$SERVER_PID    : " + serverPid + "   (this JVM is pid " + pid
                    + (serverPid == pid ? " -- the engine runs IN this process" : "") + ")");
            for (int i = 0; i < raw.size(); i++) {
                System.out.println((i == 0 ? "    info version      : " : "                        ")
                        + raw.get(i));
            }
        }
    }

    public static void main(String[] args) {
        // jaybird-native finds libfbclient through JNA's search path.
        System.setProperty("jna.library.path", "/opt/firebird/lib");
        String remote = args.length > 0 ? args[0] : "employee";
        String local = args.length > 1 ? args[1] : FbSample.SCRATCH + "/arch_embedded_java.fdb";
        FbSample.run(() -> {
            System.out.println("One JDBC driver: its own wire client, then one libfbclient with two providers.");
            System.out.println();
            inspect("[1] PURE_JAVA (Jaybird's own wire protocol):",
                    "jdbc:firebird://" + FbSample.HOST + "/" + remote, false);
            System.out.println();
            inspect("[2] libfbclient via JNA, inet:// -> Remote provider:",
                    "jdbc:firebird:embedded:inet://" + FbSample.HOST + "/" + remote, false);
            System.out.println();
            inspect("[3] libfbclient via JNA, local path -> Engine provider:",
                    "jdbc:firebird:embedded:" + local, true);
            System.out.println();
            System.out.println("done.");
        });
    }
}
