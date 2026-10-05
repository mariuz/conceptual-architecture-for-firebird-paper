//
// Deployment.java - the engine's own view of a deployment (Java twin of
// ../../../../../cpp/deployment.cpp; see
// ../../../../../../deployment-and-operations.md).
//
// The C++ sample's three SQL layers - MON$DATABASE (the database as
// deployed), RDB$CONFIG (the effective configuration), the SYSTEM context
// (this engine, this session) - plus the two info APIs a driver can reach
// without SQL.  Jaybird has no typed wrappers for most info items, but it
// hands out the raw calls on its GDS-ng layer:
//   - FbDatabase.getDatabaseInfo(items, len): isc_info_page_size,
//     isc_info_ods_version, isc_info_num_buffers, isc_info_forced_writes...
//     as {item, 2-byte length, little-endian value} clumplets;
//   - FBServiceManager.attachServiceManager() -> FbService.getServiceInfo():
//     the install tree an operator would otherwise read from a shell on the
//     server (home, security database, lock and message directories, the
//     databases attached right now).
// Both are decoded here by hand, which is what libfbclient's callers do.
//
// Read-only: safe to run against the shared employee database (default).
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Deployment
//
package fbsamples;

import static fbsamples.FbSample.scalar;

import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.SQLException;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.ServiceRequestBuffer;
import org.firebirdsql.gds.VaxEncoding;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbService;
import org.firebirdsql.jaybird.fb.constants.SpbItems;
import org.firebirdsql.jdbc.FirebirdConnection;
import org.firebirdsql.management.FBServiceManager;

public final class Deployment {

    private static void line(String label, Object value) {
        System.out.printf("  %-22s %s%n", label, FbSample.text(value));
    }

    private static void line(Connection con, String label, String sql) throws SQLException {
        line(label, scalar(con, sql));
    }

    /** Find an integer item in an isc_info_* reply. */
    private static long dbInfo(byte[] buf, int item) {
        int p = 0;
        while (p < buf.length && buf[p] != ISCConstants.isc_info_end) {
            int it = buf[p++] & 0xff;
            int len = VaxEncoding.iscVaxInteger2(buf, p);
            p += 2;
            if (it == item) {
                return VaxEncoding.iscVaxLong(buf, p, len);
            }
            p += len;
        }
        return -1;
    }

    private static void databaseInfo(Connection con) throws SQLException {
        FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
        byte[] items = {
            ISCConstants.isc_info_ods_version, ISCConstants.isc_info_ods_minor_version,
            ISCConstants.isc_info_page_size, ISCConstants.isc_info_num_buffers,
            ISCConstants.isc_info_sweep_interval, ISCConstants.isc_info_forced_writes,
            ISCConstants.isc_info_allocation, ISCConstants.isc_info_end};
        byte[] r = db.getDatabaseInfo(items, 256);
        System.out.println();
        System.out.println("== FbDatabase.getDatabaseInfo: isc_info_* items, no SQL ==");
        line("ODS", dbInfo(r, ISCConstants.isc_info_ods_version) + "."
                + dbInfo(r, ISCConstants.isc_info_ods_minor_version));
        line("page size", dbInfo(r, ISCConstants.isc_info_page_size));
        line("page buffers", dbInfo(r, ISCConstants.isc_info_num_buffers));
        line("sweep interval", dbInfo(r, ISCConstants.isc_info_sweep_interval));
        line("forced writes", dbInfo(r, ISCConstants.isc_info_forced_writes));
        line("pages allocated", dbInfo(r, ISCConstants.isc_info_allocation));
        line("(driver properties)", "wireCrypt=" + db.getConnectionProperties().getWireCrypt()
                + ", protocol " + db.getServerVersion().getProtocolVersion()
                + ", encrypted " + db.getServerVersion().isWireEncryptionUsed());
    }

    private static void serviceInfo() throws SQLException {
        FBServiceManager sm = new FBServiceManager();
        sm.setServerName(FbSample.HOST);
        sm.setUser(FbSample.USER);
        sm.setPassword(FbSample.PASSWORD);
        System.out.println();
        System.out.println("== FbService.getServiceInfo: the install tree, from service_mgr ==");
        try (FbService svc = sm.attachServiceManager()) {
            ServiceRequestBuffer srb = svc.createServiceRequestBuffer();
            int[] items = {
                ISCConstants.isc_info_svc_server_version, ISCConstants.isc_info_svc_implementation,
                ISCConstants.isc_info_svc_get_env, ISCConstants.isc_info_svc_user_dbpath,
                ISCConstants.isc_info_svc_get_env_lock, ISCConstants.isc_info_svc_get_env_msg,
                ISCConstants.isc_info_svc_svr_db_info};
            String[] labels = {"server version", "architecture", "home directory", "security database",
                "lock directory", "message directory"};
            for (int it : items) {
                srb.addArgument(it);
            }
            byte[] r = svc.getServiceInfo(null, srb, 4096);
            int p = 0;
            while (p < r.length && r[p] != ISCConstants.isc_info_end) {
                int it = r[p++] & 0xff;
                if (it == ISCConstants.isc_info_svc_svr_db_info) {
                    long att = 0;
                    long dbs = 0;
                    StringBuilder names = new StringBuilder();
                    while (p < r.length && (r[p] & 0xff) != ISCConstants.isc_info_flag_end) {
                        int sub = r[p++] & 0xff;
                        if (sub == ISCConstants.isc_spb_num_att || sub == ISCConstants.isc_spb_num_db) {
                            long v = VaxEncoding.iscVaxInteger(r, p, 4);
                            p += 4;
                            if (sub == ISCConstants.isc_spb_num_att) {
                                att = v;
                            } else {
                                dbs = v;
                            }
                        } else if (sub == SpbItems.isc_spb_dbname) {
                            int len = VaxEncoding.iscVaxInteger2(r, p);
                            p += 2;
                            names.append(' ').append(new String(r, p, len, StandardCharsets.UTF_8));
                            p += len;
                        } else {
                            break;
                        }
                    }
                    p++;    // isc_info_flag_end
                    line("attached now", att + " attachments, " + dbs + " databases:" + names);
                    continue;
                }
                int len = VaxEncoding.iscVaxInteger2(r, p);
                p += 2;
                String value = new String(r, p, len, StandardCharsets.UTF_8);
                p += len;
                for (int i = 0; i < labels.length; i++) {
                    if (items[i] == it) {
                        line(labels[i], value);
                    }
                }
            }
        }
    }

    public static void main(String[] args) {
        String database = args.length > 0 ? args[0] : "employee";
        FbSample.run(() -> {
            try (Connection con = FbSample.attach(database)) {
                System.out.println("== MON$DATABASE: the database as deployed ==");
                line(con, "database file", "select mon$database_name from mon$database");
                line(con, "ODS version", "select mon$ods_major || '.' || mon$ods_minor from mon$database");
                line(con, "page size", "select mon$page_size from mon$database");
                line(con, "page buffers", "select mon$page_buffers from mon$database");
                line(con, "sweep interval", "select mon$sweep_interval from mon$database");
                line(con, "forced writes", "select mon$forced_writes from mon$database");
                line(con, "SQL dialect", "select mon$sql_dialect from mon$database");
                line(con, "crypt state", "select mon$crypt_state from mon$database");

                System.out.println();
                System.out.println("== RDB$CONFIG: effective configuration (selected of "
                        + scalar(con, "select count(*) from rdb$config") + " settings) ==");
                Windows.print(con, "select rdb$config_name, rdb$config_value, rdb$config_is_set"
                        + " from rdb$config where rdb$config_name in ('ServerMode', 'DefaultDbCachePages',"
                        + " 'DatabaseAccess', 'WireCrypt', 'MaxParallelWorkers', 'SecurityDatabase')"
                        + " order by rdb$config_name");

                System.out.println();
                System.out.println("== settings explicitly set in config files ==");
                Windows.print(con, "select rdb$config_name, rdb$config_value, rdb$config_source"
                        + " from rdb$config where rdb$config_is_set order by rdb$config_id");

                System.out.println();
                System.out.println("== SYSTEM context: this engine, this session ==");
                for (String v : new String[] {"ENGINE_VERSION", "DB_NAME", "NETWORK_PROTOCOL",
                    "WIRE_CRYPT_PLUGIN", "CLIENT_ADDRESS"}) {
                    line(con, v, "select rdb$get_context('SYSTEM', '" + v + "') from rdb$database");
                }

                databaseInfo(con);
                serviceInfo();     // while this attachment is still open
            }
            System.out.println();
            System.out.println("done.");
        });
    }
}
