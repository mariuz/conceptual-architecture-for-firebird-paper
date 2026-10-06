//
// LockManager.cs - the three lck_wait outcomes and a deadlock, timed (C#
// twin of ../../cpp/lock_manager.cpp; see ../../../lock-manager.md).
//
// A holder reserves t1 FOR PROTECTED WRITE - a genuine LCK_relation lock at
// LCK_EX - and a second attachment probes it with NO WAIT, LOCK TIMEOUT 3
// and WAIT.  A second act builds a real wait-for cycle through LCK_tra
// locks and clocks the periodic deadlock scan (DeadlockTimeout, 10 s).
//
// FirebirdClient's managed wire layer expresses the reservation in the TPB
// itself, as typed options rather than SQL or raw bytes:
// FbTransactionOptions.LockTables maps "T1" to LockWrite | Protected
// (isc_tpb_lock_write "T1" + isc_tpb_protected), TransactionBehavior
// carries NoWait / Wait, and WaitTimeout = 3 s becomes isc_tpb_lock_timeout.
// BeginTransaction either returns (granted) or throws FbException - the
// lock is taken when the transaction starts, before any statement runs.
//
// Run:  cd samples/csharp && dotnet run -- LockManager [database]
//
using System.Diagnostics;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class LockManager
{
    /// <summary>A TPB reserving T1 FOR PROTECTED WRITE with the given wait mode.</summary>
    static FbTransactionOptions ReserveT1(FbTransactionBehavior wait, TimeSpan? timeout = null) => new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency | FbTransactionBehavior.Write | wait,
        WaitTimeout = timeout,
        LockTables = new Dictionary<string, FbTransactionBehavior>
        {
            ["T1"] = FbTransactionBehavior.LockWrite | FbTransactionBehavior.Protected,
        },
    };

    static readonly FbTransactionOptions SnapshotWait = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency
                              | FbTransactionBehavior.Write
                              | FbTransactionBehavior.Wait,
    };

    static string FirstLine(FbException e) => ErrorText(e).Split('\n')[0];

    static void Probe(FbConnection con, string label, FbTransactionOptions tpb)
    {
        var sw = Stopwatch.StartNew();
        try
        {
            using var t = con.BeginTransaction(tpb);
            Console.WriteLine($"{label,-16} granted after {sw.Elapsed.TotalSeconds:F3} s");
            t.Commit();
        }
        catch (FbException e)
        {
            Console.WriteLine($"{label,-16} failed after {sw.Elapsed.TotalSeconds:F3} s: {FirstLine(e)} (gds {e.ErrorCode})");
        }
    }

    public static void Run(string[] args)
    {
        var path = DbPath("lock_manager", args);
        using var a = AttachOrCreate(path);
        using var b = Attach(path);

        Execute(a, "recreate table t1 (id int primary key, v int)");
        Execute(a, "insert into t1 values (1, 0)");
        Execute(a, "insert into t1 values (2, 0)");

        // --- act one: LCK_relation at LCK_EX ------------------------------------
        var hold = a.BeginTransaction(ReserveT1(FbTransactionBehavior.Wait));
        Console.WriteLine("holder: t1 reserved FOR PROTECTED WRITE (LCK_relation at LCK_EX)");

        Probe(b, "NO WAIT:", ReserveT1(FbTransactionBehavior.NoWait));
        Probe(b, "LOCK TIMEOUT 3:", ReserveT1(FbTransactionBehavior.Wait, TimeSpan.FromSeconds(3)));

        // WAIT parks in wait_for_request until the holder lets go.
        var releaser = Task.Run(() =>
        {
            Thread.Sleep(2000);
            hold.Commit();
            hold.Dispose();
            Console.WriteLine("holder: committed (2 s later) -> lock released");
        });
        Probe(b, "WAIT:", ReserveT1(FbTransactionBehavior.Wait));
        releaser.Wait();

        // --- act two: a wait-for cycle through LCK_tra -------------------------
        Console.WriteLine("building deadlock: A updates row 1, B updates row 2, then cross...");
        using var ta = a.BeginTransaction(SnapshotWait);
        using var tb = b.BeginTransaction(SnapshotWait);
        Execute(a, "update t1 set v = v + 1 where id = 1", ta);
        Execute(b, "update t1 set v = v + 1 where id = 2", tb);

        var sw = Stopwatch.StartNew();
        var aVictim = false;
        var crossA = Task.Run(() =>
        {
            try
            {
                Execute(a, "update t1 set v = v + 1 where id = 2", ta);
            }
            catch (FbException e)                       // A was chosen as victim
            {
                Console.WriteLine($"deadlock: A failed after {sw.Elapsed.TotalSeconds:F1} s: {FirstLine(e)}");
                aVictim = true;
                ta.Rollback();                          // free B: the victim's tx still holds its locks
            }
        });
        Thread.Sleep(300);
        var bVictim = false;
        try
        {
            Execute(b, "update t1 set v = v + 1 where id = 1", tb);
            Console.WriteLine($"deadlock: B's update proceeded after {sw.Elapsed.TotalSeconds:F1} s (A was the victim)");
        }
        catch (FbException e)
        {
            Console.WriteLine($"deadlock: B failed after {sw.Elapsed.TotalSeconds:F1} s: {FirstLine(e)}");
            bVictim = true;
            tb.Rollback();                              // free A
        }
        crossA.Wait();
        if (!aVictim) ta.Rollback();
        if (!bVictim) tb.Rollback();
        Console.WriteLine("the wait is DeadlockTimeout (10 s default): the cycle sat undetected until the scan.");
    }
}
