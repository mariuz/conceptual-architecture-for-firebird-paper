//
// GcSweep.cs - record versions, the collectors and sweep, watched through
// MON$RECORD_STATS (C# twin of ../../cpp/gc_sweep.cpp; see
// ../../../garbage-collection-and-sweep.md).
//
// On a fresh database: pin a SNAPSHOT, commit twelve updates under it,
// release it and scan (intermediate GC, purge), delete and scan (expunge),
// then roll back a no_auto_undo transaction to freeze the OIT - and sweep.
// FirebirdClient reaches every lever without leaving its public API: the
// pin and the stump are FbTransactionBehavior flags (Concurrency, and
// NoAutoUndo - the isc_tpb_no_auto_undo item itself), the four header
// counters are FbDatabaseInfo calls (isc_info_oldest_transaction / _active
// / _snapshot / next_transaction over op_info_database, so peeking starts
// no transaction), and the sweep is FbValidation with
// FbValidationFlags.SweepDatabase - the Services API's gfix -sweep, run
// inside the server.
//
// Run:  cd samples/csharp && dotnet run -- GcSweep [database]
//
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Services;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class GcSweep
{
    static readonly FbTransactionOptions SnapshotPin = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency | FbTransactionBehavior.Read
                              | FbTransactionBehavior.Wait,
    };

    static readonly FbTransactionOptions Stump = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency | FbTransactionBehavior.Write
                              | FbTransactionBehavior.NoWait | FbTransactionBehavior.NoAutoUndo,
    };

    /// <summary>MON$ is a snapshot per transaction: each auto-committed query is a fresh one.</summary>
    static void ShowStats(FbConnection con, string label)
    {
        var s = Rows(con, "select r.MON$RECORD_UPDATES, r.MON$RECORD_IMGC, r.MON$RECORD_PURGES, "
                        + "       r.MON$RECORD_EXPUNGES, r.MON$BACKVERSION_READS "
                        + "from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)")[0];
        Console.WriteLine($"{label,-34} upd={s[0],-4} imgc={s[1],-3} purges={s[2],-3} "
                          + $"expunges={s[3],-3} backreads={s[4]}");
    }

    /// <summary>The header counters as database info items: no transaction needed.</summary>
    static void ShowCounters(FbConnection con, string label)
    {
        var i = new FbDatabaseInfo(con);
        Console.WriteLine($"{label,-34} OIT={i.GetOldestTransaction()} OAT={i.GetOldestActiveTransaction()} "
                          + $"OST={i.GetOldestActiveSnapshot()} Next={i.GetNextTransaction()} "
                          + $"(sweep interval {i.GetSweepInterval()})");
    }

    /// <summary>Drop-and-create; this provider version throws NullReferenceException
    /// (not FbException) when DropDatabase finds no file, so catch both.</summary>
    static FbConnection Fresh(string path)
    {
        var cs = Builder(path).ToString();
        try { FbConnection.DropDatabase(cs); }
        catch (Exception e) when (e is FbException or NullReferenceException) { /* did not exist */ }
        FbConnection.CreateDatabase(cs, overwrite: true);
        return Attach(path);
    }

    public static void Run(string[] args)
    {
        var path = DbPath("gc_sweep", args);
        using var writer = Fresh(path);
        using var pinner = Attach(path);
        Execute(writer, "create table gctest (id int primary key, val int)");
        Execute(writer, "insert into gctest values (1, 0)");

        // 1. Pin a snapshot: its tra_oldest_active holds the OST down.
        const string read = "select val from gctest where id = 1";
        using var snap = pinner.BeginTransaction(SnapshotPin);
        Console.WriteLine($"pinned SNAPSHOT reads val = {Scalar(pinner, read, snap)}");
        ShowStats(writer, "before updates:");

        // 2. Twelve committed updates (auto-commit: one transaction each).
        using (var upd = new FbCommand("update gctest set val = @val where id = 1", writer))
        {
            var val = upd.Parameters.Add("@val", FbDbType.Integer);
            for (var i = 1; i <= 12; i++)
            {
                val.Value = i;
                upd.ExecuteNonQuery();
            }
        }
        ShowStats(writer, "after 12 updates (snapshot open):");
        Console.WriteLine($"pinned SNAPSHOT still reads val = {Scalar(pinner, read, snap)}");

        // 3. Release the snapshot; a scan trips over the below-OST chain.
        snap.Commit();
        Console.WriteLine($"snapshot released; new reader sees val = {Scalar(writer, read)}");
        Thread.Sleep(1500);
        ShowStats(writer, "after release + scan + 1.5s:");

        // 4. A committed DELETE older than the OST is expunged, not purged.
        Execute(writer, "delete from gctest where id = 1");
        Scalar(writer, "select count(*) from gctest");
        Thread.Sleep(1500);
        ShowStats(writer, "after DELETE + scan + 1.5s:");

        // 5. A no_auto_undo rollback leaves a stump that pins the OIT.
        ShowCounters(writer, "header counters before rollback:");
        using (var stump = writer.BeginTransaction(Stump))
        {
            Execute(writer, "insert into gctest values (2, 0)", stump);
            stump.Rollback();
        }
        ShowCounters(writer, "after no_auto_undo rollback:");

        // 6. The sweep the C++ sample can only recommend: gfix -sweep as a service.
        var sweep = new FbValidation(ServiceConnectionString(path))
        {
            Options = FbValidationFlags.SweepDatabase,
        };
        sweep.Execute();
        ShowCounters(writer, "after FbValidation SweepDatabase:");
    }
}
