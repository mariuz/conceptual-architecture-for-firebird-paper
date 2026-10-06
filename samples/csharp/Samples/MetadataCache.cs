//
// MetadataCache.cs - the metadata cache's visibility rule from two
// attachments (C# twin of ../../cpp/metadata_cache.cpp; see
// ../../../metadata-cache.md).
//
//   1. an uncommitted ALTER is visible to its own transaction only;
//   2. a committed ALTER is visible at once, even to a statement prepared
//      inside B's older, still-open SNAPSHOT: metadata is read-committed;
//   3. two concurrent uncommitted DDLs on one object collide in
//      CacheElement::newVersion ("object in use");
//   4. every committed ALTER left a row in RDB$FORMATS.
//
// ADO.NET makes "which transaction did this prepare run in" explicit twice
// over: an FbConnection holds at most one BeginTransaction at a time, and
// while it does, every FbCommand must name that FbTransaction or the
// provider refuses to run it.  The status vector arrives as
// FbException.Errors, one FbError (message + gds Number) per element.
//
// Run:  cd samples/csharp && dotnet run -- MetadataCache [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class MetadataCache
{
    static readonly FbTransactionOptions Snapshot = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency
                              | FbTransactionBehavior.Write
                              | FbTransactionBehavior.Wait,
    };

    static readonly FbTransactionOptions ReadCommitted = new()
    {
        TransactionBehavior = FbTransactionBehavior.ReadCommitted
                              | FbTransactionBehavior.RecVersion
                              | FbTransactionBehavior.Write
                              | FbTransactionBehavior.NoWait,
    };

    static void TryQuery(string who, FbConnection con, FbTransaction tx, string sql)
    {
        try
        {
            Console.WriteLine($"{who}: {sql} -> {Text(Scalar(con, sql, tx))}");
        }
        catch (FbException e)
        {
            Console.WriteLine($"{who}: {sql} -> ERROR: {ErrorText(e).Replace("\n", "; ")}");
        }
    }

    public static void Run(string[] args)
    {
        var path = DbPath("mdc", args);
        using var a = AttachOrCreate(path);
        using var b = Attach(path);
        Execute(a, "recreate table t (a integer)");
        Execute(a, "insert into t values (1)");

        // -- 1. uncommitted DDL: mine, and mine alone ------------------------------
        Console.WriteLine("== 1. uncommitted ALTER: visible to creator only ==");
        var ta = a.BeginTransaction(ReadCommitted);
        Execute(a, "alter table t add e integer", ta);           // stays uncommitted
        TryQuery("A (same tx)  ", a, ta, "select e from t");
        using (var tb = b.BeginTransaction(ReadCommitted))
        {
            TryQuery("B            ", b, tb, "select e from t");
            tb.Commit();
        }

        // -- 2. committed DDL ignores open snapshots -------------------------------
        Console.WriteLine();
        Console.WriteLine("== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==");
        using (var snapB = b.BeginTransaction(Snapshot))         // isc_tpb_concurrency
        {
            TryQuery("B (snapshot) ", b, snapB, "select count(*) from t");
            ta.Commit();                                         // E becomes committed
            ta.Dispose();
            using (var td = a.BeginTransaction(ReadCommitted))
            {
                Execute(a, "alter table t add d integer", td);
                td.Commit();                                     // D committed after B's snapshot
            }
            TryQuery("B (same  tx) ", b, snapB, "select d from t");
            Console.WriteLine("   (records are snapshot-isolated; metadata is read-committed -");
            Console.WriteLine("    the new statement was prepared against the chain's current head)");
            snapB.Commit();
        }

        // -- 3. concurrent DDL: the newVersion collision ---------------------------
        Console.WriteLine();
        Console.WriteLine("== 3. two uncommitted DDLs on one object ==");
        using (var tf = a.BeginTransaction(ReadCommitted))
        using (var tg = b.BeginTransaction(ReadCommitted))
        {
            Execute(a, "alter table t add f integer", tf);
            try
            {
                Execute(b, "alter table t add g integer", tg);
                Console.WriteLine("B: ALTER unexpectedly succeeded");
            }
            catch (FbException e)
            {
                Console.WriteLine("B: ALTER failed:");
                Console.WriteLine(ErrorText(e));
                var codes = string.Join(", ", e.Errors.Select(x => x.Number).Where(n => n != 0).Distinct());
                Console.WriteLine($"   (SQLSTATE {e.SQLSTATE}, {e.Errors.Count} FbError entries, gds codes [{codes}])");
            }
            tg.Rollback();
            tf.Rollback();                                       // F vanishes with the rollback
        }

        // -- 4. the on-disk half: one format per committed shape -------------------
        Console.WriteLine();
        Console.WriteLine("== 4. RDB$FORMATS after the committed DDL ==");
        var formats = Scalar(a, "select count(*) from rdb$formats f "
                                + "join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id "
                                + "where r.rdb$relation_name = 'T'");
        Console.WriteLine($"formats stored for T: {formats} (T has lived through that many shapes)");
        Console.WriteLine($"{"A",-2} {"E",-6} D");
        foreach (var r in Rows(a, "select a, e, d from t"))
            Console.WriteLine($"{Text(r[0]),-2} {Text(r[1]),-6} {Text(r[2])}");

        // -- the ADO.NET rule that makes the boundaries explicit -------------------
        using (var open = a.BeginTransaction(ReadCommitted))
        {
            try
            {
                Execute(a, "select 1 from rdb$database");        // no transaction named
            }
            catch (InvalidOperationException e)
            {
                Console.WriteLine();
                Console.WriteLine("ADO.NET: a command without its transaction is refused:");
                Console.WriteLine("   " + e.Message);
            }
            open.Rollback();
        }
        Console.WriteLine("done.");
    }
}
