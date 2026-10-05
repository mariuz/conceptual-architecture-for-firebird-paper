//
// LockManager.java - the three lck_wait outcomes and a deadlock, timed
// (Java twin of ../../../../../cpp/lock_manager.cpp; see
// ../../../../../../lock-manager.md).
//
// A holder reserves t1 FOR PROTECTED WRITE - a genuine LCK_relation lock at
// LCK_EX - and a second attachment probes it with NO WAIT, LOCK TIMEOUT 3
// and WAIT.  A second act builds a real wait-for cycle through LCK_tra
// locks and clocks the periodic deadlock scan (DeadlockTimeout, 10 s).
//
// Jaybird reaches the reservation both ways the other twins do, through its
// wire-level GDS-ng layer (FirebirdConnection.getFbDatabase()):
// FbDatabase.startTransaction(String) runs "SET TRANSACTION ... RESERVING"
// (fb-cpp's idiom) for the holder, and startTransaction(tpb) takes a TPB
// built item by item - isc_tpb_lock_write "T1" + isc_tpb_protected, plus
// isc_tpb_nowait / isc_tpb_lock_timeout 3 / isc_tpb_wait - for the probes
// (fbintf's and firebird-driver's idiom).  Both start the transaction, and
// so take the lock, before any statement runs.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=LockManager
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.sql.Connection;
import java.sql.SQLException;
import java.util.concurrent.atomic.AtomicBoolean;

import org.firebirdsql.gds.TransactionParameterBuffer;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.jaybird.fb.constants.TpbItems;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class LockManager {

    private static double since(long t0) {
        return (System.nanoTime() - t0) / 1e9;
    }

    /** First line of an engine error, without Jaybird's [SQLState/ISC] suffix. */
    private static String firstLine(SQLException e) {
        String m = e.getMessage();
        int nl = m.indexOf('\n');
        if (nl >= 0) {
            m = m.substring(0, nl);
        }
        int br = m.indexOf(" [SQLState:");
        if (br >= 0) {
            m = m.substring(0, br);
        }
        int semi = m.indexOf(';');
        return semi >= 0 ? m.substring(0, semi) : m;
    }

    /** A TPB reserving T1 FOR PROTECTED WRITE with the given wait item(s). */
    private static TransactionParameterBuffer reserveT1(FbDatabase db, int waitItem, int timeout) {
        TransactionParameterBuffer tpb = db.createTransactionParameterBuffer();
        tpb.addArgument(TpbItems.isc_tpb_concurrency);
        tpb.addArgument(TpbItems.isc_tpb_write);
        if (waitItem == TpbItems.isc_tpb_lock_timeout) {
            tpb.addArgument(TpbItems.isc_tpb_wait);   // a timeout is a timed WAIT
            tpb.addArgument(TpbItems.isc_tpb_lock_timeout, timeout);
        } else {
            tpb.addArgument(waitItem);
        }
        tpb.addArgument(TpbItems.isc_tpb_lock_write, "T1");
        tpb.addArgument(TpbItems.isc_tpb_protected);
        return tpb;
    }

    private static void probe(FbDatabase db, String label, TransactionParameterBuffer tpb) {
        long t0 = System.nanoTime();
        try {
            FbTransaction t = db.startTransaction(tpb);
            System.out.printf("%-16s granted after %.3f s%n", label, since(t0));
            t.commit();
        } catch (SQLException e) {
            System.out.printf("%-16s failed after %.3f s: %s (gds %d)%n",
                    label, since(t0), firstLine(e), e.getErrorCode());
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("lock_manager", args);
            try (Connection a = attachOrCreate(path); Connection b = attach(path)) {
                execute(a, "recreate table t1 (id int primary key, v int)");
                execute(a, "insert into t1 values (1, 0)");
                execute(a, "insert into t1 values (2, 0)");

                FbDatabase dbA = a.unwrap(FirebirdConnection.class).getFbDatabase();
                FbDatabase dbB = b.unwrap(FirebirdConnection.class).getFbDatabase();

                // --- act one: LCK_relation at LCK_EX ----------------------------
                FbTransaction hold = dbA.startTransaction(
                        "SET TRANSACTION WAIT RESERVING t1 FOR PROTECTED WRITE");
                System.out.println("holder: t1 reserved FOR PROTECTED WRITE (LCK_relation at LCK_EX)");

                probe(dbB, "NO WAIT:", reserveT1(dbB, TpbItems.isc_tpb_nowait, 0));
                probe(dbB, "LOCK TIMEOUT 3:", reserveT1(dbB, TpbItems.isc_tpb_lock_timeout, 3));

                // WAIT parks in wait_for_request until the holder lets go.
                Thread releaser = new Thread(() -> {
                    try {
                        Thread.sleep(2000);
                        hold.commit();
                        System.out.println("holder: committed (2 s later) -> lock released");
                    } catch (Exception e) {
                        System.out.println("holder: commit failed: " + e);
                    }
                });
                releaser.start();
                probe(dbB, "WAIT:", reserveT1(dbB, TpbItems.isc_tpb_wait, 0));
                releaser.join();

                // --- act two: a wait-for cycle through LCK_tra -------------------
                System.out.println("building deadlock: A updates row 1, B updates row 2, then cross...");
                for (Connection c : new Connection[] {a, b}) {
                    c.setAutoCommit(false);
                    c.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ); // SNAPSHOT WAIT
                }
                execute(a, "update t1 set v = v + 1 where id = 1");
                execute(b, "update t1 set v = v + 1 where id = 2");

                AtomicBoolean aVictim = new AtomicBoolean();
                long t0 = System.nanoTime();
                Thread crossA = new Thread(() -> {
                    try {
                        execute(a, "update t1 set v = v + 1 where id = 2");
                    } catch (SQLException e) {          // A was chosen as victim
                        System.out.printf("deadlock: A failed after %.1f s: %s%n", since(t0), firstLine(e));
                        aVictim.set(true);
                        try {
                            a.rollback();               // free B
                        } catch (SQLException ignored) {
                            // nothing more to do
                        }
                    }
                });
                crossA.start();
                Thread.sleep(300);
                boolean bVictim = false;
                try {
                    execute(b, "update t1 set v = v + 1 where id = 1");
                    System.out.printf("deadlock: B's update proceeded after %.1f s (A was the victim)%n",
                            since(t0));
                } catch (SQLException e) {
                    System.out.printf("deadlock: B failed after %.1f s: %s%n", since(t0), firstLine(e));
                    bVictim = true;
                    b.rollback();                       // free A
                }
                crossA.join();
                if (!aVictim.get()) {
                    a.rollback();
                }
                if (!bVictim) {
                    b.rollback();
                }
                System.out.println("the wait is DeadlockTimeout (10 s default): the cycle sat "
                        + "undetected until the scan.");
            }
        });
    }
}
