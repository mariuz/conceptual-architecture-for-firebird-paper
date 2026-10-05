//
// Transactions.java - MVCC isolation seen from the client (Java twin of
// ../../../../../cpp/transactions_demo.cpp; see
// ../../../../../../transactions-and-concurrency.md).
//
// Two attachments play the same scenario: SNAPSHOT stability, READ
// COMMITTED freshness, then a NO WAIT write conflict.  JDBC's isolation
// levels map onto Firebird TPBs (TRANSACTION_REPEATABLE_READ is
// isc_tpb_concurrency, TRANSACTION_READ_COMMITTED is read_committed +
// rec_version, TRANSACTION_SERIALIZABLE is consistency), all WAIT by
// default - but Jaybird lets you replace the TPB behind each level:
// FirebirdConnection.createTransactionParameterBuffer() takes the same
// isc_tpb_* items the C++ sample packs by hand, and
// setTransactionParameters(level, tpb) installs them.  So the conflict
// fails fast, as in the C++ and Python twins.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Transactions
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.SQLException;

import org.firebirdsql.gds.TransactionParameterBuffer;
import org.firebirdsql.jaybird.fb.constants.TpbItems;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class Transactions {

    private static final String SELECT = "select amount from balance where id = 1";

    /** Replace the TPB behind REPEATABLE_READ and READ_COMMITTED with NO WAIT ones. */
    private static void noWait(Connection con) throws SQLException {
        FirebirdConnection fc = con.unwrap(FirebirdConnection.class);

        TransactionParameterBuffer snapshot = fc.createTransactionParameterBuffer();
        snapshot.addArgument(TpbItems.isc_tpb_concurrency);
        snapshot.addArgument(TpbItems.isc_tpb_write);
        snapshot.addArgument(TpbItems.isc_tpb_nowait);
        fc.setTransactionParameters(Connection.TRANSACTION_REPEATABLE_READ, snapshot);

        TransactionParameterBuffer readCommitted = fc.createTransactionParameterBuffer();
        readCommitted.addArgument(TpbItems.isc_tpb_read_committed);
        readCommitted.addArgument(TpbItems.isc_tpb_rec_version);
        readCommitted.addArgument(TpbItems.isc_tpb_write);
        readCommitted.addArgument(TpbItems.isc_tpb_nowait);
        fc.setTransactionParameters(Connection.TRANSACTION_READ_COMMITTED, readCommitted);
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("tx", args);
            try (Connection a = attachOrCreate(path); Connection b = attach(path)) {
                execute(a, "recreate table balance (id integer primary key, amount integer)");
                execute(a, "insert into balance values (1, 100)");
                noWait(a);
                noWait(b);

                // --- 1. SNAPSHOT stability -----------------------------------
                a.setAutoCommit(false);
                a.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ);
                System.out.println("A (SNAPSHOT)       sees amount = " + scalar(a, SELECT));

                execute(b, "update balance set amount = 999 where id = 1");
                System.out.println("B                  committed amount = 999");

                System.out.println("A (same SNAPSHOT)  sees amount = " + scalar(a, SELECT)
                        + "   <- still the start-of-tx version");
                a.commit();

                // --- 2. READ COMMITTED sees the new version --------------------
                a.setTransactionIsolation(Connection.TRANSACTION_READ_COMMITTED);
                System.out.println("A (READ COMMITTED) sees amount = " + scalar(a, SELECT)
                        + "   <- the committed version");
                a.commit();

                // --- 3. Write conflict under NO WAIT ---------------------------
                b.setAutoCommit(false);
                b.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ);
                execute(b, "update balance set amount = amount + 1 where id = 1");

                a.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ);
                try {
                    execute(a, "update balance set amount = amount + 10 where id = 1");
                    System.out.println("unexpected: conflicting update succeeded");
                } catch (SQLException e) {
                    System.out.println("A conflicting update failed as designed:");
                    System.out.println("    " + FbSample.errorText(e).replace("\n", "\n    "));
                    System.out.println("    SQLState " + e.getSQLState() + " / gds " + e.getErrorCode());
                }
                a.rollback();
                b.commit();
            }
            System.out.println("done.");
        });
    }
}
