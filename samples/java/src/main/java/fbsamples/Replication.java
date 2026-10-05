//
// Replication.java - the client-visible half of replication: the
// publication, and the replica switch (Java twin of
// ../../../../../cpp/replication.cpp; see
// ../../../../../../replication-architecture.md).
//
// Plain DDL walks the one publication per database through its states,
// read back from RDB$PUBLICATIONS / RDB$PUBLICATION_TABLES:
//   ALTER DATABASE ENABLE PUBLICATION, INCLUDE TABLE ..., INCLUDE ALL.
// The journal/segment transport needs server-side replication.conf and
// stays as text in the document.
//
// The replica end is reached two ways.  Reading: the fb_info_replica_mode
// database-info item, sent through Jaybird's GDS-ng
// FbDatabase.getDatabaseInfo and decoded by hand.  Writing: Jaybird 6.0.6's
// FBMaintenanceManager has no replica-mode setter, but it is an open class
// with protected Services hooks (createRequestBuffer,
// executeServicesOperation), so a small subclass sends
// isc_action_svc_properties + isc_spb_prp_replica_mode - the gfix -replica
// switch - like the Go driver's MaintenanceManager.SetReplicaMode.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Replication
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.rows;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.ServiceRequestBuffer;
import org.firebirdsql.gds.VaxEncoding;
import org.firebirdsql.gds.ng.FbService;
import org.firebirdsql.jdbc.FirebirdConnection;
import org.firebirdsql.management.FBMaintenanceManager;

public final class Replication {

    /** FBMaintenanceManager plus the replica-mode property it lacks. */
    static final class ReplicaManager extends FBMaintenanceManager {
        void setReplicaMode(int mode) throws SQLException {
            try (FbService service = attachServiceManager()) {
                ServiceRequestBuffer srb = createRequestBuffer(service, ISCConstants.isc_action_svc_properties, 0);
                srb.addArgument(ISCConstants.isc_spb_prp_replica_mode, (byte) mode);
                executeServicesOperation(service, srb);
            }
        }
    }

    private static void pubState(Connection con, String when) throws SQLException {
        System.out.println("-- " + when);
        Object[] pub = rows(con, "select trim(rdb$publication_name), rdb$active_flag, rdb$auto_enable "
                + "from rdb$publications").get(0);
        List<String> tables = new ArrayList<>();
        for (Object[] r : rows(con, "select trim(rdb$table_schema_name) || '.' || trim(rdb$table_name) "
                + "from rdb$publication_tables order by rdb$table_name")) {
            tables.add((String) r[0]);
        }
        System.out.printf("%-13s ACTIVE_FLAG %s   AUTO_ENABLE %s    published: %s%n", pub[0], pub[1], pub[2],
                tables.isEmpty() ? "(none)" : String.join(", ", tables));
    }

    /** The fb_info_replica_mode database-info item, decoded by hand. */
    private static int infoReplicaMode(Connection con) throws SQLException {
        byte[] b = con.unwrap(FirebirdConnection.class).getFbDatabase().getDatabaseInfo(
                new byte[] {(byte) ISCConstants.fb_info_replica_mode, ISCConstants.isc_info_end}, 16);
        if (b[0] != (byte) ISCConstants.fb_info_replica_mode) {
            throw new SQLException("unexpected info item " + b[0]);
        }
        int len = VaxEncoding.iscVaxInteger2(b, 1);
        return VaxEncoding.iscVaxInteger(b, 3, len);
    }

    private static void replicaState(Connection con, String note) throws SQLException {
        System.out.printf("MON$REPLICA_MODE = %s, fb_info_replica_mode = %d  (%s)%n",
                scalar(con, "select mon$replica_mode from mon$database"), infoReplicaMode(con), note);
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("replication", args);
            try (Connection con = attachOrCreate(path)) {
                // Idempotent reset (auto-commit: a failed statement dooms only itself).
                for (String sql : new String[] {"alter database exclude all from publication",
                        "alter database disable publication", "drop table repl_orders", "drop table repl_scratch"}) {
                    try {
                        execute(con, sql);
                    } catch (SQLException ignored) {
                        // nothing to undo
                    }
                }
                execute(con, "create table repl_orders (id int not null primary key, item varchar(30))");
                execute(con, "create table repl_scratch (n int)");          // note: no key

                pubState(con, "initial state (publication exists but is inactive)");
                execute(con, "alter database enable publication");
                pubState(con, "after ENABLE PUBLICATION");
                execute(con, "alter database include table repl_orders to publication");
                pubState(con, "after INCLUDE TABLE REPL_ORDERS");
                execute(con, "alter database include all to publication");
                pubState(con, "after INCLUDE ALL (auto-enable: future tables join automatically)");
                replicaState(con, "0 = not a replica: this side publishes");
            }

            ReplicaManager mm = new ReplicaManager();
            mm.setServerName(FbSample.HOST);
            mm.setUser(FbSample.USER);
            mm.setPassword(FbSample.PASSWORD);
            mm.setDatabase(path);

            System.out.println();
            System.out.println("-- Services: isc_spb_prp_replica_mode = isc_spb_prp_rm_readonly");
            mm.setReplicaMode(ISCConstants.isc_spb_prp_rm_readonly);
            try (Connection rep = attach(path)) {
                replicaState(rep, "1 = read-only replica");
                try {
                    execute(rep, "insert into repl_orders values (1, 'widget')");
                    System.out.println("BUG: user INSERT on a read-only replica succeeded");
                } catch (SQLException e) {
                    String m = e.getMessage();
                    int br = m.indexOf(" [SQLState:");
                    System.out.println("user INSERT on the replica: " + (br >= 0 ? m.substring(0, br) : m)
                            + " (gds " + e.getErrorCode() + ")");
                }
            }
            System.out.println("-- Services: isc_spb_prp_replica_mode = isc_spb_prp_rm_none");
            mm.setReplicaMode(ISCConstants.isc_spb_prp_rm_none);
            try (Connection con = attach(path)) {
                replicaState(con, "a primary again");
            }
            System.out.println("done.");
        });
    }
}
