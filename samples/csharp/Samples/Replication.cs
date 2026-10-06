//
// Replication.cs - the client-visible half of replication: the publication
// and the replica-mode flag (C# twin of ../../cpp/replication.cpp; see
// ../../../replication-architecture.md).
//
// Plain DDL walks the one publication per database through its states,
// read back from RDB$PUBLICATIONS / RDB$PUBLICATION_TABLES:
//   ALTER DATABASE ENABLE PUBLICATION, INCLUDE TABLE ..., INCLUDE ALL.
// The journal/segment transport needs server-side replication.conf and
// stays as text in the document.
//
// The replica end is read two ways: MON$DATABASE.MON$REPLICA_MODE, and the
// fb_info_replica_mode database-info item, which FirebirdClient decodes
// itself (FbDatabaseInfo.GetReplicaMode()) - no hand-decoding of the info
// buffer as in the Java twin.  Writing it is out of reach: FbConfiguration
// has no replica-mode setter, and unlike Jaybird's FBMaintenanceManager the
// Services classes cannot be extended - the request-building hooks are
// internal - so isc_spb_prp_replica_mode (gfix -replica) cannot be sent.
//
// Run:  cd samples/csharp && dotnet run -- Replication [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Replication
{
    static void PubState(FbConnection con, string when)
    {
        Console.WriteLine("-- " + when);
        var pub = Rows(con, "select trim(rdb$publication_name), rdb$active_flag, rdb$auto_enable from rdb$publications")[0];
        var tables = Rows(con, "select trim(rdb$table_schema_name) || '.' || trim(rdb$table_name) "
                               + "from rdb$publication_tables order by rdb$table_name")
            .Select(r => Text(r[0])).ToList();
        Console.WriteLine($"{Text(pub[0]),-13} ACTIVE_FLAG {Text(pub[1])}   AUTO_ENABLE {Text(pub[2])}    published: "
                          + (tables.Count == 0 ? "(none)" : string.Join(", ", tables)));
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("replication", args));

        // Idempotent reset (auto-commit: a failed statement dooms only itself).
        foreach (var sql in new[] { "alter database exclude all from publication", "alter database disable publication",
                                    "drop table repl_orders", "drop table repl_scratch" })
        {
            try { Execute(con, sql); } catch (FbException) { /* nothing to undo */ }
        }
        Execute(con, "create table repl_orders (id int not null primary key, item varchar(30))");
        Execute(con, "create table repl_scratch (n int)");                // note: no key

        PubState(con, "initial state (publication exists but is inactive)");
        Execute(con, "alter database enable publication");
        PubState(con, "after ENABLE PUBLICATION");
        Execute(con, "alter database include table repl_orders to publication");
        PubState(con, "after INCLUDE TABLE REPL_ORDERS");
        Execute(con, "alter database include all to publication");
        PubState(con, "after INCLUDE ALL (auto-enable: future tables join automatically)");

        Console.WriteLine();
        Console.WriteLine($"MON$REPLICA_MODE = {Text(Scalar(con, "select mon$replica_mode from mon$database"))}, "
                          + $"FbDatabaseInfo.GetReplicaMode() = {new FbDatabaseInfo(con).GetReplicaMode()}"
                          + "  (not a replica: this side publishes)");
        Console.WriteLine("done.");
    }
}
