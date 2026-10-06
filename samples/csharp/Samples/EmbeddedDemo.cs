//
// EmbeddedDemo.cs - the full server engine, loaded into the .NET process (C#
// twin of ../../cpp/embedded_demo.cpp; see
// ../../../embedded-architecture-comparison.md).
//
// FirebirdClient's default is a managed wire client, but ServerType=Embedded
// makes it P/Invoke libfbclient (ClientLibrary), and a plain local path
// makes the Y-valve load the Engine provider (plugins/libEngine14.so) into
// this process.  The three demonstrations of the C++ sample:
//   1. /proc/self/maps before and after: nothing Firebird in the process
//      until the first embedded attach, then libfbclient AND libEngine14;
//   2. real work with no server: DDL, DML and a query against a .fdb this
//      process created (NETWORK_PROTOCOL is NULL, the engine's pid is ours);
//   3. the continuum measured: attach+detach timed embedded, through the
//      same libfbclient to the server, and through the provider's own
//      managed wire client - three connection strings, one API (and the
//      last once more with employee held open by another attachment).
// The remote libfbclient leg is Database = "[::1]:employee" with
// ISC_USER / ISC_PASSWORD set in the C environment: the provider's parser
// splits "inet://..." into DataSource + Database and the embedded path then
// drops DataSource, and the native path sends no password in the DPB (see
// ArchitectureComparison.cs).  Pooling is off throughout, so every Close()
// is a real detach.
//
// Run:  cd samples/csharp && dotnet run -- EmbeddedDemo [path]
//
using System.Diagnostics;
using System.Runtime.InteropServices;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class EmbeddedDemo
{
    const string ClientLibrary = "/opt/firebird/lib/libfbclient.so";

    [DllImport("libc", EntryPoint = "setenv")]
    static extern int SetEnv(string name, string value, int overwrite);

    static string Mapped(string fragment) => File.ReadAllText("/proc/self/maps").Contains(fragment) ? "yes" : "no";

    static void State(string label) =>
        Console.WriteLine($"{label,-15} libfbclient mapped={Mapped("libfbclient")}, libEngine14 mapped={Mapped("libEngine14")}");

    static string Native(string database, string charset = "UTF8")
    {
        var b = Builder(database, charset);
        b.ServerType = FbServerType.Embedded;
        b.ClientLibrary = ClientLibrary;
        return b.ToString();
    }

    static double AttachMs(string cs)
    {
        var sw = Stopwatch.StartNew();
        using (var con = new FbConnection(cs))
            con.Open();
        return sw.Elapsed.TotalMilliseconds;
    }

    public static void Run(string[] args)
    {
        var local = args.Length > 0 ? args[0] : $"{Scratch}/embedded_demo_cs.fdb";
        SetEnv("ISC_USER", User, 1);
        SetEnv("ISC_PASSWORD", Password, 1);
        var embedded = Native(local);
        var viaClient = Native("[::1]:employee", "NONE");
        var managed = Builder("employee", "NONE").ToString();

        // --- 1. watch the engine arrive in our address space -----------------
        State("before attach:");
        if (!File.Exists(local))
            FbConnection.CreateDatabase(embedded, overwrite: false);
        using (var con = new FbConnection(embedded))
        {
            con.Open();
            State("after  attach:");
            Console.WriteLine();

            // --- 2. real work with no server anywhere ------------------------
            Execute(con, "recreate table gadgets (id int primary key, name varchar(20))");
            Execute(con, "insert into gadgets values (1, 'sprocket')");
            Execute(con, "insert into gadgets values (2, 'flange')");
            Execute(con, "insert into gadgets values (3, 'grommet')");
            var row = Rows(con,
                "select count(*), max(name)," +
                " coalesce(rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), '<null: in-process>')," +
                " a.mon$server_pid" +
                " from gadgets, mon$attachments a" +
                " where a.mon$attachment_id = current_connection group by 3, 4")[0];
            Console.WriteLine($"rows={row[0]}  max(name)={Text(row[1])}  NETWORK_PROTOCOL={Text(row[2])}");
            Console.WriteLine($"engine pid={row[3]}, my pid={Environment.ProcessId} - the 'server' is this process");
            Console.WriteLine();

            // --- 3. attach cost, the working attachment still open ------------
            (string Label, string Shown, string Cs)[] legs =
            {
                ("embedded", $"ServerType=Embedded, {local}", embedded),
                ("libfbclient -> server", "ServerType=Embedded, [::1]:employee", viaClient),
                ("managed wire client", "ServerType=Default, localhost:employee", managed),
            };
            const int runs = 5;
            var sum = new double[legs.Length];
            foreach (var leg in legs) AttachMs(leg.Cs);   // warm-up: JIT, library loading, auth code paths
            for (var i = 0; i < runs; i++)
                for (var l = 0; l < legs.Length; l++)
                    sum[l] += AttachMs(legs[l].Cs);
            Console.WriteLine($"attach+detach avg over {runs} runs:");
            for (var l = 0; l < legs.Length; l++)
                Console.WriteLine($"    {legs[l].Label,-30} {legs[l].Shown,-55} {sum[l] / runs,7:F2} ms");

            // employee was opened cold by every remote attach above; keep it open
            using var keep = new FbConnection(managed);
            keep.Open();
            double kept = 0;
            for (var i = 0; i < runs; i++) kept += AttachMs(managed);
            Console.WriteLine($"    {"managed, employee kept open",-30} {legs[2].Shown,-55} {kept / runs,7:F2} ms");
        }
        Console.WriteLine("done.");
    }
}
