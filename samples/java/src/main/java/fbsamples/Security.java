//
// Security.java - the security layers observed from client code (Java twin
// of ../../../../../cpp/security.cpp; see
// ../../../../../../security-architecture.md).
//
// The same four steps: the attachment's own MON$ATTACHMENTS row (layers 1+2
// as the server recorded them), SEC$USERS, a temporary user plus a role
// carrying MONITOR_ANY_ATTACHMENT (attached without and then with the role),
// and a failed login.  Jaybird's default PURE_JAVA protocol implements
// layers 1 and 2 itself - its own Srp256 client proof and its own wire
// encryption, which offers only "ChaCha" (and "Arc4"), not fbclient's
// "ChaCha64" - so the cipher the server records differs from the
// fbclient-based twins'.  The role is the roleName connection property (the
// DPB's isc_dpb_sql_role_name); the wrong password arrives as an
// SQLException carrying SQLState 28000 and gds 335544472 (isc_login).
// Names JAVA_USER / JAVA_MONITOR let it run beside the other twins.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Security
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.rows;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.util.Properties;

public final class Security {

    private static final String USER = "JAVA_USER";
    private static final String PASS = "Hands0nPw";
    private static final String ROLE = "JAVA_MONITOR";

    private static void whoAmI(Connection con, String label) throws SQLException {
        Object[] r = rows(con, "select trim(mon$user), mon$auth_method, mon$wire_crypt_plugin,"
                + " mon$remote_protocol, trim(coalesce(current_role, 'NONE'))"
                + " from mon$attachments where mon$attachment_id = current_connection").get(0);
        System.out.printf("%-22s user=%s auth=%s wirecrypt=%s protocol=%s role=%s%n",
                label, r[0], r[1], r[2], r[3], r[4]);
    }

    private static Object visible(Connection con) throws SQLException {
        return scalar(con, "select count(*) from mon$attachments where mon$system_flag = 0");
    }

    /**
     * Cleanup of a previous run, only for what exists: DROP USER of a missing
     * user fails only AT COMMIT (user management is deferred work), where
     * Jaybird also logs the transaction's unexpected COMMITTING state.
     */
    private static void cleanup(Connection con) throws SQLException {
        if (((Number) scalar(con, "select count(*) from sec$users where sec$user_name = '"
                + USER + "'")).intValue() > 0) {
            execute(con, "drop user " + USER + " using plugin Srp");
            con.commit();
        }
        if (((Number) scalar(con, "select count(*) from rdb$roles where rdb$role_name = '"
                + ROLE + "'")).intValue() > 0) {
            execute(con, "drop role " + ROLE);
        }
        con.commit();
    }

    private static Connection as(String path, String user, String pass, String role) throws SQLException {
        Properties p = FbSample.props();
        p.setProperty("user", user);
        p.setProperty("password", pass);
        if (role != null) {
            p.setProperty("roleName", role);       // -> isc_dpb_sql_role_name
        }
        return DriverManager.getConnection(FbSample.url(path), p);
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("security", args);
            try (Connection admin = attachOrCreate(path)) {
                // 1. Layers 1+2, as recorded for THIS attachment.
                whoAmI(admin, "admin attachment:");

                // 2+3. User management is deferred work executed at commit:
                // explicit transactions, one full commit per DDL batch (JDBC
                // auto-commit would do full commits too; explicit ones make the
                // batch boundaries visible).
                admin.setAutoCommit(false);
                cleanup(admin);
                execute(admin, "create user " + USER + " password '" + PASS + "' using plugin Srp");
                admin.commit();                      // <- the user exists only now
                execute(admin, "create role " + ROLE
                        + " set system privileges to MONITOR_ANY_ATTACHMENT");
                execute(admin, "grant " + ROLE + " to user " + USER);
                admin.commit();

                System.out.println();
                System.out.println("SEC$USERS (the security database, through the virtual view):");
                System.out.printf("    %-16s %-8s %s%n", "USER", "PLUGIN", "ADMIN");
                for (Object[] r : rows(admin, "select trim(sec$user_name), trim(sec$plugin), sec$admin"
                        + " from sec$users order by 1")) {
                    System.out.printf("    %-16s %-8s %s%n", r[0], r[1], r[2]);
                }

                System.out.println();
                System.out.println("admin sees " + visible(admin) + " user attachments in MON$ATTACHMENTS");
                admin.commit();

                try (Connection plain = as(path, USER, PASS, null)) {
                    whoAmI(plain, "user, no role:");
                    System.out.println("  -> sees " + visible(plain) + " attachment(s): only its own");
                }
                try (Connection monitor = as(path, USER, PASS, ROLE)) {
                    whoAmI(monitor, "user + role:");
                    System.out.println("  -> sees " + visible(monitor)
                            + " attachments: MONITOR_ANY_ATTACHMENT at work");
                }

                // 4. The failed login.
                System.out.println();
                System.out.println("failed login (wrong password) produces:");
                try (Connection bad = as(path, USER, "wrong-password", null)) {
                    System.out.println("    unexpected: login succeeded");
                } catch (SQLException e) {
                    System.out.println("    SQLState " + e.getSQLState() + " / gds " + e.getErrorCode()
                            + " (" + e.getClass().getSimpleName() + ")");
                    System.out.println("    " + FbSample.errorText(e));
                }

                execute(admin, "drop user " + USER + " using plugin Srp");
                execute(admin, "drop role " + ROLE);
                admin.commit();
            }
            System.out.println();
            System.out.println("temporary user and role dropped. done.");
        });
    }

    private Security() {
    }
}
