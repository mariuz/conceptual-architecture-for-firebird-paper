//
// Protocol.java - the negotiated wire session, from both ends (Java twin of
// ../../../../../protocol_client.cpp, ../../../../python/protocol.py and
// ../../../../go/protocol/main.go; see ../../../../../../firebird-wire-protocol.md).
//
// Jaybird's default PURE_JAVA protocol is an independent implementation of
// everything in the wire document: op_connect with a version list (up to
// protocol 19 in Jaybird 6), op_cond_accept, Srp256 / Srp / Legacy_Auth,
// op_crypt with ChaCha or Arc4, and zlib wire compression - Java code, no
// libfbclient.  The sample attaches, prints the C++ sample's SYSTEM-context
// answers and the MON$ATTACHMENTS row the server recorded, then the
// client's own view: the isc_info_firebird_version lines Jaybird keeps
// (GDSServerVersion), whose /P19:C suffix carries the negotiated protocol
// and the C(rypt) / Z(lib) flags.
//
// Because the handshake is the driver's code, connection properties steer
// it: wireCompression=true adds zlib on top of the cipher (the engine
// records it), authPlugins=Srp offers only the SHA-1 proof and is refused
// (this server's AuthServer is Srp256 alone), and wireCrypt=DISABLED is
// refused because the server's WireCrypt is Required.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Protocol
//
package fbsamples;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.util.Properties;

import org.firebirdsql.gds.impl.GDSServerVersion;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class Protocol {

    private static String ctx(Connection con, String name) throws SQLException {
        Object v = FbSample.scalar(con, "select rdb$get_context('SYSTEM', '" + name + "') from rdb$database");
        return v == null ? "(none)" : v.toString();
    }

    private static void session(String title, String database, String... extra) {
        System.out.println("== " + title + " ==");
        Properties p = FbSample.props(extra);
        p.remove("charSet");
        p.setProperty("encoding", "NONE");     // stock employee.fdb is charset NONE
        String url = "jdbc:firebird://" + FbSample.HOST + "/" + database;
        try (Connection con = DriverManager.getConnection(url, p)) {
            System.out.println("attached to " + url);
            System.out.println("engine version : " + ctx(con, "ENGINE_VERSION"));
            System.out.println("protocol       : " + ctx(con, "NETWORK_PROTOCOL"));
            System.out.println("wire crypt     : " + ctx(con, "WIRE_CRYPT_PLUGIN"));
            System.out.println("authenticated  : " + FbSample.scalar(con, "select trim(current_user) from rdb$database"));
            Object[] a = FbSample.rows(con, "select mon$auth_method, mon$remote_version, mon$wire_crypt_plugin,"
                    + " mon$wire_compressed, mon$client_version"
                    + " from mon$attachments where mon$attachment_id = current_connection").get(0);
            System.out.println("MON$ATTACHMENTS, as the server recorded the handshake:");
            System.out.println("   auth method    : " + FbSample.text(a[0]));
            System.out.println("   wire protocol  : " + FbSample.text(a[1]));
            System.out.println("   wire crypt     : " + FbSample.text(a[2]));
            System.out.println("   compressed     : " + FbSample.text(a[3]));
            System.out.println("   client version : " + FbSample.text(a[4]));
            GDSServerVersion sv = con.unwrap(FirebirdConnection.class).getFbDatabase().getServerVersion();
            System.out.println("the client side (isc_info_firebird_version, kept by Jaybird):");
            for (String line : sv.getRawVersions()) {
                System.out.println("   " + line);
            }
            System.out.println("   -> protocol " + sv.getProtocolVersion() + ", encrypted "
                    + sv.isWireEncryptionUsed() + ", compressed " + sv.isWireCompressionUsed());
        } catch (SQLException e) {
            System.out.println("attach refused : " + e.getMessage());
        }
        System.out.println();
    }

    public static void main(String[] args) {
        String database = args.length > 0 ? args[0] : "employee";
        FbSample.run(() -> {
            session("default properties: Jaybird's preferences", database);
            session("wireCompression=true: zlib on top of the cipher", database, "wireCompression=true");
            session("authPlugins=Srp: the client offers only the SHA-1 proof", database, "authPlugins=Srp");
            session("wireCrypt=DISABLED: the client refuses to encrypt", database, "wireCrypt=DISABLED");
            System.out.println("detached. bye");
        });
    }
}
