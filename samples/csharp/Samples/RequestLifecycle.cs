//
// RequestLifecycle.cs - one CREATE TABLE round trip, instrumented from the
// client (C# twin of ../../cpp/request_lifecycle.cpp; see
// ../../../request-lifecycle-code-trace.md).
//
// Each API step is timed and the attachment's own counters are sampled
// around it:
//   prepare -> DSQL picks DsqlDdlStatement (statement type DDL)
//   execute -> catalog STOREs: the record-insert counter jumps, and the new
//              RDB$RELATIONS row is visible to this transaction only
//   commit  -> TRA_commit -> DFW -> CCH_flush -> PIO_write: page writes
//
// Stage 2's client half is FirebirdClient's managed C# wire protocol, and
// ADO.NET exposes the stages one call each: BeginTransaction, Prepare,
// ExecuteNonQuery, Commit.  What it does not expose is the prepare's
// verdict - the statement type is internal to the provider - so the DDL
// classification shows up only indirectly (no result columns, -1 records
// affected).  And while an FbConnection holds a transaction, every command
// on it must use that transaction, so the MON$ samples cannot run beside
// it on the same attachment as they do in the Java twin: a second
// attachment reads the first one's MON$IO_STATS / MON$RECORD_STATS (by
// attachment id, each sample in a fresh auto-commit transaction) and
// doubles as the outside observer.  FbDatabaseInfo looks like a
// transaction-free shortcut but is not one here: its fetches / marks /
// writes are database-wide (the engine answers them from dbb_stats), and
// GetInsertCount() overflows converting the per-table reply to one Int32.
//
// Run:  cd samples/csharp && dotnet run -- RequestLifecycle [database]
//
using System.Diagnostics;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class RequestLifecycle
{
    const string Seen = "select count(*) from rdb$relations where rdb$relation_name = 'TRACE_DEMO'";

    static readonly FbTransactionOptions SnapshotWait = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency
                              | FbTransactionBehavior.Write
                              | FbTransactionBehavior.Wait,
    };

    /// <summary>fetches, marks, writes, record inserts of attachment attId, read by the observer.</summary>
    static long[] Sample(FbConnection observer, long attId) =>
        Rows(observer, "select i.mon$page_fetches, i.mon$page_marks, i.mon$page_writes,"
                       + "       r.mon$record_inserts"
                       + " from mon$attachments a"
                       + " join mon$io_stats i on a.mon$stat_id = i.mon$stat_id"
                       + " join mon$record_stats r on a.mon$stat_id = r.mon$stat_id"
                       + $" where a.mon$attachment_id = {attId}")[0]
            .Select(Convert.ToInt64).ToArray();

    static double Ms(Stopwatch sw) => sw.Elapsed.TotalMilliseconds;

    public static void Run(string[] args)
    {
        var path = DbPath("request_lifecycle", args);
        using var con = AttachOrCreate(path);
        using var observer = Attach(path);
        try { Execute(con, "drop table trace_demo"); } catch (FbException) { /* first run */ }
        var attId = Convert.ToInt64(Scalar(con, "select current_connection from rdb$database"));

        var s0 = Sample(observer, attId);
        using var tx = con.BeginTransaction(SnapshotWait);
        try
        {
            using var cmd = new FbCommand("CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY, name VARCHAR(30))", con, tx);

            // -- prepare: Y-valve -> remote -> DSQL (Stages 1-5) -------------------
            var sw = Stopwatch.StartNew();
            cmd.Prepare();
            Console.WriteLine($"prepare  {Ms(sw),6:F2} ms   statement type: not exposed by the provider (no plan: '{cmd.GetCommandPlan()?.Trim()}')");

            // -- execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) --------
            sw.Restart();
            var affected = cmd.ExecuteNonQuery();
            var tExec = Ms(sw);
            var s1 = Sample(observer, attId);
            Console.WriteLine($"execute  {tExec,6:F2} ms   records affected = {affected}; catalog record inserts: +{s1[3] - s0[3]}, page marks: +{s1[1] - s0[1]}");
            Console.WriteLine($"         in this tx:  RDB$RELATIONS has TRACE_DEMO = {Scalar(con, Seen, tx)}");
            Console.WriteLine($"         other att:   RDB$RELATIONS has TRACE_DEMO = {Scalar(observer, Seen)}  (TRA_commit has not happened)");

            // -- commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9) -----
            sw.Restart();
            tx.Commit();
            var tCommit = Ms(sw);
            var s2 = Sample(observer, attId);
            Console.WriteLine($"commit   {tCommit,6:F2} ms   page writes: +{s2[2] - s1[2]}  (fetches: +{s2[0] - s0[0]} over the whole trip)");
            Console.WriteLine($"         other att:   RDB$RELATIONS has TRACE_DEMO = {Scalar(observer, Seen)}");
        }
        catch (FbException)
        {
            tx.Rollback();
            throw;
        }
        Console.WriteLine("done.");
    }
}
