//
// RequestLifecycle.java - one CREATE TABLE round trip, instrumented from
// the client (Java twin of ../../../../../cpp/request_lifecycle.cpp; see
// ../../../../../../request-lifecycle-code-trace.md).
//
// Each API step is timed and the attachment's own MON$IO_STATS /
// MON$RECORD_STATS counters are sampled around it:
//   prepare -> DSQL picks DsqlDdlStatement (statement type DDL)
//   execute -> catalog STOREs: the record-insert counter jumps, and the new
//              RDB$RELATIONS row is visible to this transaction only
//   commit  -> TRA_commit -> DFW -> CCH_flush -> PIO_write: page writes
//
// Here Stage 2's client half is Java: Jaybird's pure-Java wire protocol.
// JDBC itself would hide the stages (a Connection runs one transaction,
// and Statement.execute prepares and executes in one call), so the trip
// runs on Jaybird's GDS-ng layer, one level down: FbDatabase.
// startTransaction, createStatement, FbStatement.prepare - whose getType()
// returns the typed StatementType.DDL - then execute and FbTransaction.
// commit, each its own wire operation.  An attachment carries any number of
// transactions, so the MON$ samples and the outside visibility check run
// as plain JDBC auto-commit queries on the *same* Connection, each in a
// fresh transaction (MON$ snapshots are frozen per transaction).
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=RequestLifecycle
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.rows;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;

import org.firebirdsql.gds.TransactionParameterBuffer;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbStatement;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.gds.ng.fields.RowValue;
import org.firebirdsql.gds.ng.listeners.StatementListener;
import org.firebirdsql.jaybird.fb.constants.TpbItems;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class RequestLifecycle {

    private static final String SEEN = "select count(*) from rdb$relations where rdb$relation_name = 'TRACE_DEMO'";

    /** fetches, marks, writes, record inserts of this attachment, in a fresh (auto-commit) tx. */
    private static long[] sample(Connection con) throws SQLException {
        Object[] r = rows(con, "select i.mon$page_fetches, i.mon$page_marks, i.mon$page_writes,"
                + "       r.mon$record_inserts"
                + " from mon$attachments a"
                + " join mon$io_stats i on a.mon$stat_id = i.mon$stat_id"
                + " join mon$record_stats r on a.mon$stat_id = r.mon$stat_id"
                + " where a.mon$attachment_id = current_connection").get(0);
        long[] s = new long[4];
        for (int i = 0; i < 4; i++) {
            s[i] = ((Number) r[i]).longValue();
        }
        return s;
    }

    /** A one-value query inside a given GDS-ng transaction. */
    private static long countIn(FbDatabase db, FbTransaction tra, String sql) throws SQLException {
        FbStatement st = db.createStatement(tra);
        try {
            List<RowValue> rows = new ArrayList<>();
            st.addStatementListener(new StatementListener() {
                @Override
                public void receivedRow(FbStatement sender, RowValue row) {
                    rows.add(row);
                }
            });
            st.prepare(sql);
            st.execute(RowValue.EMPTY_ROW_VALUE);
            st.fetchRows(1);
            return st.getRowDescriptor().getFieldDescriptor(0).getDatatypeCoder()
                    .decodeLong(rows.get(0).getFieldData(0));
        } finally {
            st.close();
        }
    }

    private static double ms(long t0) {
        return (System.nanoTime() - t0) / 1e6;
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("request_lifecycle", args);
            try (Connection con = attachOrCreate(path)) {
                try {
                    execute(con, "drop table trace_demo");          // leftover from a previous run
                } catch (SQLException ignored) {
                    // first run
                }
                FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();

                long[] s0 = sample(con);
                TransactionParameterBuffer tpb = db.createTransactionParameterBuffer();
                tpb.addArgument(TpbItems.isc_tpb_concurrency);
                tpb.addArgument(TpbItems.isc_tpb_write);
                tpb.addArgument(TpbItems.isc_tpb_wait);
                FbTransaction tra = db.startTransaction(tpb);
                FbStatement stmt = db.createStatement(tra);
                try {
                    // -- prepare: Y-valve -> remote -> DSQL (Stages 1-5) -----------
                    long t0 = System.nanoTime();
                    stmt.prepare("CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY, name VARCHAR(30))");
                    System.out.printf("prepare  %6.2f ms   statement type = %s%n", ms(t0), stmt.getType());

                    // -- execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) -
                    t0 = System.nanoTime();
                    stmt.execute(RowValue.EMPTY_ROW_VALUE);
                    double tExec = ms(t0);
                    long[] s1 = sample(con);
                    System.out.printf("execute  %6.2f ms   catalog record inserts: +%d, page marks: +%d%n",
                            tExec, s1[3] - s0[3], s1[1] - s0[1]);
                    System.out.println("         in this tx:  RDB$RELATIONS has TRACE_DEMO = " + countIn(db, tra, SEEN));
                    System.out.println("         other tx:    RDB$RELATIONS has TRACE_DEMO = " + scalar(con, SEEN)
                            + "  (TRA_commit has not happened)");
                    stmt.close();

                    // -- commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9)
                    t0 = System.nanoTime();
                    tra.commit();
                    double tCommit = ms(t0);
                    long[] s2 = sample(con);
                    System.out.printf("commit   %6.2f ms   page writes: +%d  (fetches: +%d over the whole trip)%n",
                            tCommit, s2[2] - s1[2], s2[0] - s0[0]);
                    System.out.println("         other tx:    RDB$RELATIONS has TRACE_DEMO = " + scalar(con, SEEN));
                } catch (SQLException e) {
                    tra.rollback();
                    throw e;
                }
            }
            System.out.println("done.");
        });
    }
}
