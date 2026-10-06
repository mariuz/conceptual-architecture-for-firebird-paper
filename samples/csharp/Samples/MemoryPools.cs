//
// MemoryPools.cs - the pool hierarchy made visible from SQL via
// MON$MEMORY_USAGE (C# twin of ../../cpp/memory_pools.cpp; see
// ../../../memory-management.md).
//
// The per-level summary with the parent-redirection signature (child pools
// with real MON$MEMORY_USED and zero MON$MEMORY_ALLOCATED), the worker's
// database -> attachment -> transaction chain, and a transaction pool
// growing under an uncommitted 3000-row UPDATE and dying with its
// rollback - watched from a second attachment, because a MON$ snapshot is
// frozen per transaction (so every monitor read runs in a transaction of
// its own).  The ids come from SQL (CURRENT_CONNECTION /
// CURRENT_TRANSACTION), as in the C++ sample: FirebirdClient's
// FbDatabaseInfo wraps many isc_info_* items but not isc_info_attachment_id,
// and FbTransaction exposes no transaction id.  What FbDatabaseInfo does
// give is the engine's own memory counters over the managed wire protocol:
// GetCurrentMemory() / GetMaxMemory() (isc_info_current_memory /
// isc_info_max_memory), printed beside the database pool's MON$ row.  The
// SUMs over BIGINT arrive as INT128, i.e. System.Numerics.BigInteger.
//
// Run:  cd samples/csharp && dotnet run -- MemoryPools [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class MemoryPools
{
    /// <summary>A query in a transaction of its own: a fresh MON$ snapshot each time.</summary>
    static List<object?[]> Snapshot(FbConnection mon, string sql)
    {
        using var tx = mon.BeginTransaction();
        var rows = Rows(mon, sql, tx);
        tx.Commit();
        return rows;
    }

    static void LevelSummary(FbConnection mon)
    {
        Console.WriteLine("GROUP POOLS USED       ALLOCATED  WITH_OWN_EXTENTS");
        foreach (var r in Snapshot(mon,
                     "select MON$STAT_GROUP, count(*), sum(MON$MEMORY_USED), "
                   + "       sum(MON$MEMORY_ALLOCATED), count(nullif(MON$MEMORY_ALLOCATED, 0)) "
                   + "from MON$MEMORY_USAGE group by 1 order by 1"))
            Console.WriteLine($"{Text(r[0]),-5} {Text(r[1]),-5} {Text(r[2]),-10} {Text(r[3]),-10} {Text(r[4])}");
    }

    /// <summary>One pool's used/allocated, from a fresh monitor snapshot.</summary>
    static long PoolRow(FbConnection mon, string label, string join)
    {
        var rows = Snapshot(mon, "select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE " + join);
        if (rows.Count == 0)
            return -1;
        Console.WriteLine($"  {label,-24} used={Text(rows[0][0]),-10} allocated={Text(rows[0][1])}");
        return Convert.ToInt64(rows[0][0]);
    }

    public static void Run(string[] args)
    {
        var path = DbPath("memory_pools", args);
        using var worker = AttachOrCreate(path);
        using var mon = Attach(path);

        Execute(worker, "recreate table t (id int, pad varchar(200))");
        Execute(worker, "execute block as declare i int = 0; begin"
                      + "  while (i < 3000) do begin"
                      + "    insert into t values (:i, rpad('x', 200, 'x')); i = i + 1;"
                      + "  end "
                      + "end");

        Console.WriteLine("-- per-level summary (0=db 1=att 2=tra 3=stmt 5=cmp; "
                          + "used > 0 with allocated = 0: parent redirection)");
        LevelSummary(mon);

        // The worker's own chain: database -> attachment -> transaction.
        using var tx = worker.BeginTransaction();
        var ids = Rows(worker, "select current_connection, current_transaction from rdb$database", tx)[0];
        var att = Convert.ToInt64(ids[0]);
        var tra = Convert.ToInt64(ids[1]);

        Console.WriteLine($"\n-- worker's pool chain (attachment {att}, transaction {tra}; before the update)");
        PoolRow(mon, "database pool:", "join MON$DATABASE using (MON$STAT_ID)");
        var info = new FbDatabaseInfo(worker);
        Console.WriteLine($"  (FbDatabaseInfo: current memory {info.GetCurrentMemory()}, max {info.GetMaxMemory()})");
        var attJoin = $"join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = {att}";
        var traJoin = $"join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = {tra}";
        PoolRow(mon, "worker attachment pool:", attJoin);
        PoolRow(mon, "worker transaction pool:", traJoin);

        // Grow the transaction pool: the undo log lives in it.
        Execute(worker, "update t set pad = rpad('y', 200, 'y')", tx);
        Console.WriteLine("\n-- after an uncommitted 3000-row UPDATE in that transaction");
        var attBefore = PoolRow(mon, "worker attachment pool:", attJoin);
        var traUsed = PoolRow(mon, "worker transaction pool:", traJoin);

        tx.Rollback();      // bulk-free: the whole pool goes at once
        Console.WriteLine("\n-- after rollback (transaction pool destroyed with its undo log)");
        var attAfter = PoolRow(mon, "worker attachment pool:", attJoin);
        Console.WriteLine($"  attachment used fell by {attBefore - attAfter}; the dead transaction pool held {traUsed}");
    }
}
