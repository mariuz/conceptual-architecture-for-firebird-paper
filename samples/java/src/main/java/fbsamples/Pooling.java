//
// Pooling.java - both directions of connection pooling (Java twin of
// ../../../../../cpp/pooling.cpp; see ../../../../../../connection-pooling.md).
//
// Outbound: the server-side external-connections (EDS) pool, exactly as in
// the C++ sample - tune it with ALTER EXTERNAL CONNECTIONS POOL, make three
// EXECUTE STATEMENT ... ON EXTERNAL calls, read the pool's context
// variables.  Two stages are added: COMMIT RETAIN sent as SQL keeps the
// pooled connection active, and a block run in JDBC auto-commit mode parks
// it on the idle list at once, because Jaybird's auto-commit is a hard
// commit (the Go driver's is COMMIT RETAINING).
//
// Inbound: pooling is a big part of the Java story, but Jaybird itself
// ships no production pool.  What it provides is the JDBC plumbing pools
// are built on: FBSimpleDataSource (a plain DataSource, every close() a
// detach) and FBConnectionPoolDataSource, a javax.sql.ConnectionPoolDataSource
// whose PooledConnection is ONE physical attachment that hands out logical
// handles and fires connectionClosed when a handle is closed - the hook an
// application server's pool listens to.  HikariCP-style pools instead wrap
// a plain DataSource (or the DriverManager URL) and keep the physical
// connections themselves.  Either way the attachment survives the borrower;
// this sample shows what that means: the same CURRENT_CONNECTION comes
// back, and since Jaybird's reset only restores auto-commit and client
// info, a USER_SESSION variable set by the previous borrower is still
// there until someone issues ALTER SESSION RESET.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Pooling
//
package fbsamples;

import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.SQLException;
import java.util.List;

import javax.sql.ConnectionEvent;
import javax.sql.ConnectionEventListener;
import javax.sql.PooledConnection;

import org.firebirdsql.ds.FBConnectionPoolDataSource;

public final class Pooling {

    private static final String PROCESS = "fbsamples-pooling";

    private static void poolState(Connection con, String moment) throws SQLException {
        List<Object[]> r = FbSample.rows(con,
                "select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_SIZE'),"
                + "       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_LIFETIME'),"
                + "       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT'),"
                + "       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT')"
                + " from rdb$database");
        Object[] row = r.get(0);
        System.out.printf("%-26s size=%s lifetime=%ss idle=%s active=%s%n", moment,
                FbSample.text(row[0]), FbSample.text(row[1]), FbSample.text(row[2]), FbSample.text(row[3]));
    }

    private static void block(Connection con, String external, String moment) throws SQLException {
        String sql = "execute block returns (idle varchar(10), active varchar(10)) as\n"
                + "  declare i int = 0;\n"
                + "  declare v int;\n"
                + "begin\n"
                + "  while (i < 3) do\n"
                + "  begin\n"
                + "    execute statement 'select 1 from rdb$database'\n"
                + "      on external '" + external + "'\n"
                + "      as user '" + FbSample.USER + "' password '" + FbSample.PASSWORD + "'\n"
                + "      into :v;\n"
                + "    i = i + 1;\n"
                + "  end\n"
                + "  idle   = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');\n"
                + "  active = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');\n"
                + "  suspend;\n"
                + "end";
        Object[] row = FbSample.rows(con, sql).get(0);
        System.out.printf("%-26s idle=%s active=%s   (3 calls, 1 outbound connection)%n", moment,
                FbSample.text(row[0]), FbSample.text(row[1]));
    }

    /** Attachments this sample's process currently holds (by MON$REMOTE_PROCESS). */
    private static Object ours(Connection con) throws SQLException {
        return scalar(con, "select count(*) from mon$attachments"
                + " where mon$remote_process = '" + PROCESS + "'");
    }

    private static void outbound(String database, String external) throws SQLException {
        System.out.println("-- outbound: the server-side EDS pool --");
        try (Connection con = FbSample.attach(database)) {
            con.setAutoCommit(false);
            execute(con, "alter external connections pool set size 5");
            execute(con, "alter external connections pool set lifetime 30 second");
            con.commit();
            poolState(con, "before:");

            block(con, external, "inside the block:");
            execute(con, "commit retain");
            poolState(con, "after COMMIT RETAIN:");
            con.commit();
            poolState(con, "after commit():");

            con.setAutoCommit(true);
            block(con, external, "inside (auto-commit):");
            poolState(con, "after the auto-commit:");

            execute(con, "alter external connections pool clear all");
            poolState(con, "after CLEAR ALL:");
        }
    }

    private static void inbound(String database) throws SQLException {
        System.out.println();
        System.out.println("-- inbound: Jaybird's FBConnectionPoolDataSource --");
        FBConnectionPoolDataSource cpds = new FBConnectionPoolDataSource();
        cpds.setServerName(FbSample.HOST);
        cpds.setDatabaseName(database);
        cpds.setUser(FbSample.USER);
        cpds.setPassword(FbSample.PASSWORD);
        cpds.setEncoding("NONE");
        cpds.setProcessName(PROCESS);

        try (Connection observer = FbSample.attach(database)) {
            PooledConnection pooled = cpds.getPooledConnection();
            pooled.addConnectionEventListener(new ConnectionEventListener() {
                @Override
                public void connectionClosed(ConnectionEvent event) {
                    System.out.println("    (listener) connectionClosed: handle returned, attachment kept");
                }

                @Override
                public void connectionErrorOccurred(ConnectionEvent event) {
                    System.out.println("    (listener) connectionErrorOccurred: " + event.getSQLException());
                }
            });
            System.out.println("getPooledConnection():     " + ours(observer)
                    + " attachment(s) of " + PROCESS + " in MON$ATTACHMENTS");

            try (Connection first = pooled.getConnection()) {
                Object att = scalar(first, "select current_connection from rdb$database");
                scalar(first, "select rdb$set_context('USER_SESSION', 'BORROWER', 'first') from rdb$database");
                System.out.println("1st borrower:              CURRENT_CONNECTION = " + att
                        + ", sets USER_SESSION BORROWER = 'first'");
                System.out.println("1st borrower close():");
            }
            System.out.println("after close():             " + ours(observer) + " attachment(s) still open");

            try (Connection second = pooled.getConnection()) {
                Object[] r = FbSample.rows(second, "select current_connection,"
                        + " rdb$get_context('USER_SESSION', 'BORROWER') from rdb$database").get(0);
                System.out.println("2nd borrower:              CURRENT_CONNECTION = " + r[0]
                        + ", BORROWER = " + FbSample.text(r[1]) + "   <- same attachment, state leaked");
                execute(second, "alter session reset");
                System.out.println("after ALTER SESSION RESET: BORROWER = " + FbSample.text(scalar(second,
                        "select rdb$get_context('USER_SESSION', 'BORROWER') from rdb$database")));
            }

            pooled.close();
            System.out.println("PooledConnection.close(): " + ours(observer) + " attachment(s) left -- a real detach");
        }
    }

    public static void main(String[] args) {
        String database = args.length > 0 ? args[0] : "employee";
        String external = args.length > 1 ? args[1] : "inet://localhost/employee";
        FbSample.run(() -> {
            outbound(database, external);
            inbound(database);
            System.out.println("done.");
        });
    }
}
