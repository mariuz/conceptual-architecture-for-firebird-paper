//
// Deployment.cs - the engine's own view of a deployment (C# twin of
// ../../cpp/deployment.cpp; see ../../../deployment-and-operations.md).
//
// The C++ sample's three SQL layers - MON$DATABASE (the database as
// deployed), RDB$CONFIG (the effective configuration), the SYSTEM context
// (this engine, this session) - plus the two info APIs a driver can reach
// without SQL.  Where Jaybird hands out raw clumplets, FirebirdClient has
// typed wrappers for both:
//   - FbDatabaseInfo: one method per isc_info_* / fb_info_* item
//     (GetOdsVersion, GetPageSize, GetNumBuffers, GetForcedWrites,
//     GetWireCrypt, GetProtocolVersion, ...);
//   - FbServerProperties: the service manager's isc_info_svc_* items - the
//     install tree an operator would otherwise read from a shell on the
//     server, and the databases attached right now (GetDatabasesInfo).
//
// Read-only: safe to run against the shared employee database (default).
//
// Run:  cd samples/csharp && dotnet run -- Deployment [database]
//
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Services;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Deployment
{
    static void Line(string label, object? value) => Console.WriteLine($"  {label,-22} {Text(value)}");

    static void Print(FbConnection con, FbTransaction tx, string sql)
    {
        using var cmd = new FbCommand(sql, con, tx);
        using var r = cmd.ExecuteReader();
        var table = new List<string[]> { Enumerable.Range(0, r.FieldCount).Select(r.GetName).ToArray() };
        while (r.Read())
            table.Add(Enumerable.Range(0, r.FieldCount).Select(i => Text(r.IsDBNull(i) ? null : r.GetValue(i)).Trim()).ToArray());
        var widths = Enumerable.Range(0, r.FieldCount).Select(i => table.Max(row => row[i].Length)).ToArray();
        string Row(string[] cells) => string.Join(" ", cells.Select((c, i) => c.PadRight(widths[i]))).TrimEnd();
        Console.WriteLine(Row(table[0]));
        Console.WriteLine(Row(widths.Select(w => new string('-', w)).ToArray()));
        foreach (var row in table.Skip(1)) Console.WriteLine(Row(row));
    }

    public static void Run(string[] args)
    {
        using var con = args.Length > 0 ? Attach(args[0], "NONE") : Employee();
        using var tx = con.BeginTransaction(new FbTransactionOptions
        {
            TransactionBehavior = FbTransactionBehavior.Read | FbTransactionBehavior.Concurrency,
        });
        string Q(string sql) => Text(Scalar(con, sql, tx));

        Console.WriteLine("== MON$DATABASE: the database as deployed ==");
        Line("database file", Q("SELECT MON$DATABASE_NAME FROM MON$DATABASE"));
        Line("ODS version", Q("SELECT MON$ODS_MAJOR || '.' || MON$ODS_MINOR FROM MON$DATABASE"));
        Line("page size", Q("SELECT MON$PAGE_SIZE FROM MON$DATABASE"));
        Line("page buffers", Q("SELECT MON$PAGE_BUFFERS FROM MON$DATABASE"));
        Line("sweep interval", Q("SELECT MON$SWEEP_INTERVAL FROM MON$DATABASE"));
        Line("forced writes", Q("SELECT MON$FORCED_WRITES FROM MON$DATABASE"));
        Line("SQL dialect", Q("SELECT MON$SQL_DIALECT FROM MON$DATABASE"));
        Line("crypt state", Q("SELECT MON$CRYPT_STATE FROM MON$DATABASE"));

        Console.WriteLine($"\n== RDB$CONFIG: effective configuration (selected of {Q("SELECT COUNT(*) FROM RDB$CONFIG")} settings) ==");
        Print(con, tx,
            "SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_IS_SET FROM RDB$CONFIG " +
            "WHERE RDB$CONFIG_NAME IN ('ServerMode', 'DefaultDbCachePages', 'DatabaseAccess', " +
            "  'WireCrypt', 'MaxParallelWorkers', 'SecurityDatabase') ORDER BY RDB$CONFIG_NAME");

        Console.WriteLine("\n== settings explicitly set in config files ==");
        Print(con, tx,
            "SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_SOURCE FROM RDB$CONFIG " +
            "WHERE RDB$CONFIG_IS_SET ORDER BY RDB$CONFIG_ID");
        using (var cmd = new FbCommand("SELECT FIRST 1 RDB$CONFIG_IS_SET FROM RDB$CONFIG", con, tx))
            Console.WriteLine($"(RDB$CONFIG_IS_SET arrives as {cmd.ExecuteScalar().GetType()})");

        Console.WriteLine("\n== SYSTEM context: this engine, this session ==");
        foreach (var v in new[] { "ENGINE_VERSION", "DB_NAME", "NETWORK_PROTOCOL", "WIRE_CRYPT_PLUGIN", "CLIENT_ADDRESS" })
            Line(v, Q($"SELECT RDB$GET_CONTEXT('SYSTEM', '{v}') FROM RDB$DATABASE"));
        tx.Commit();

        Console.WriteLine("\n== FbDatabaseInfo: isc_info_* items, no SQL ==");
        var info = new FbDatabaseInfo(con);
        Line("ODS", $"{info.GetOdsVersion()}.{info.GetOdsMinorVersion()}");
        Line("page size", info.GetPageSize());
        Line("page buffers", info.GetNumBuffers());
        Line("sweep interval", info.GetSweepInterval());
        Line("forced writes", info.GetForcedWrites());
        Line("pages allocated", info.GetAllocationPages());
        Line("wire crypt plugin", $"{info.GetWireCrypt()}   (protocol {info.GetProtocolVersion()}, " +
                                  $"connection string WireCrypt={new FbConnectionStringBuilder(con.ConnectionString).WireCrypt})");
        Line("db crypt plugin", info.GetCryptPlugin() is { Length: > 0 } p ? p : "<none>");

        Console.WriteLine("\n== FbServerProperties: the install tree, from service_mgr ==");
        var props = new FbServerProperties(ServiceConnectionString());
        Line("server version", props.GetServerVersion());
        Line("architecture", props.GetImplementation());
        Line("home directory", props.GetRootDirectory());
        Line("lock directory", props.GetLockManager());
        Line("message directory", props.GetMessageFile());
        var dbs = props.GetDatabasesInfo();
        Line("attached now", $"{dbs.ConnectionCount} attachments, {dbs.Databases.Count} databases: {string.Join(", ", dbs.Databases)}");
        Console.WriteLine("\ndone.");
    }
}
