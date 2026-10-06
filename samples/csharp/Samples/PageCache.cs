//
// PageCache.cs - one hot-page ping-pong, two cache topologies (C# twin of
// ../../cpp/page_cache.cpp; see ../../../page-cache-coherency.md).
//
//   phase 1 - two client processes -> ONE SuperServer shared cache
//   phase 2 - two EMBEDDED engine processes with PRIVATE caches over one
//             file (ServerMode=SuperClassic sandbox): coherency by LCK_bdb
//             page locks and blocking ASTs, so data travels through the disk
//
// Rows 1 and 2 share a data page; each worker commits 300 updates to its own
// row, then reads its MON$IO_STATS.  The parent only launches child
// processes (this same program with a role argument).
//
// FirebirdClient carries both driver families in one assembly, chosen by a
// connection-string key: phase 1 uses the default ServerType=Default - its
// managed C# wire protocol, no native code at all - and phase 2 sets
// ServerType=Embedded + ClientLibrary=/opt/firebird/lib/libfbclient.so,
// which P/Invokes libfbclient and with it a whole engine into the child.
// The child's FIREBIRD environment variable (ProcessStartInfo.Environment)
// points that engine at a SuperClassic sandbox.
//
// Run:  cd samples/csharp && dotnet run -- PageCache
//
using System.Diagnostics;
using System.Reflection;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class PageCache
{
    const int Rounds = 300;
    static readonly string SrvDb = $"{Scratch}/page_cache_srv_cs.fdb";
    static readonly string EmbDb = $"{Scratch}/page_cache_emb_cs.fdb";
    static readonly string Sandbox = $"{Scratch}/fbemb_cs";

    static string ServerCs() => Builder(SrvDb).ToString();

    static string EmbeddedCs()
    {
        var b = Builder(EmbDb);
        b.ServerType = FbServerType.Embedded;
        b.ClientLibrary = "/opt/firebird/lib/libfbclient.so";
        b.DataSource = "";                       // a local path: the in-process engine opens the file
        return b.ToString();
    }

    static FbConnection Open(string cs)
    {
        var con = new FbConnection(cs);
        con.Open();
        return con;
    }

    static void Init(string cs)                                   // --init
    {
        FbConnection.CreateDatabase(cs, overwrite: true);     // replaces an earlier run's file
        using var con = Open(cs);
        Execute(con, "recreate table t (id int primary key, v int)");
        Execute(con, "insert into t values (1, 0)");
        Execute(con, "insert into t values (2, 0)");
    }

    static void Worker(string cs, string rowId)                   // --worker
    {
        using var con = Open(cs);
        for (var i = 0; i < Rounds; i++)                          // auto-commit: one tx each
            Execute(con, $"update t set v = v + 1 where id = {rowId}");
        var io = Rows(con, "select mon$page_fetches, mon$page_reads, mon$page_writes "
                           + "from mon$io_stats join mon$attachments using (mon$stat_id) "
                           + "where mon$attachment_id = current_connection")[0];
        Console.WriteLine($"  worker pid {Environment.ProcessId,-6} row {rowId}: {Rounds} commits | "
                          + $"page fetches={Text(io[0]),-6} reads={Text(io[1]),-4} writes={Text(io[2])}");
    }

    static void Check(string cs)                                  // --check
    {
        using var con = Open(cs);
        foreach (var r in Rows(con, "select id, v from t order by id"))
            Console.WriteLine($"  final: id={Text(r[0])} v={Text(r[1])} (expected {Rounds})");
    }

    /// <summary>Start this program again with a role argument.</summary>
    static Process Spawn(string? firebirdRoot, params string[] args)
    {
        var psi = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false };
        if (Path.GetFileNameWithoutExtension(Environment.ProcessPath) == "dotnet")
            psi.ArgumentList.Add(Assembly.GetEntryAssembly()!.Location);   // `dotnet FbSamples.dll`
        psi.ArgumentList.Add(nameof(PageCache));
        foreach (var a in args) psi.ArgumentList.Add(a);
        if (firebirdRoot != null) psi.Environment["FIREBIRD"] = firebirdRoot;
        return Process.Start(psi)!;
    }

    static void Await(params Process[] ps)
    {
        foreach (var p in ps)
        {
            using (p)
            {
                p.WaitForExit();
                if (p.ExitCode != 0) throw new InvalidOperationException($"child exited with {p.ExitCode}");
            }
        }
    }

    static void Phase(string cs, string? firebirdRoot)
    {
        Await(Spawn(firebirdRoot, "--init", cs));
        Await(Spawn(firebirdRoot, "--worker", cs, "1"), Spawn(firebirdRoot, "--worker", cs, "2"));
        Await(Spawn(firebirdRoot, "--check", cs));
    }

    /// <summary>A FIREBIRD root whose firebird.conf says SuperClassic.</summary>
    static void BuildSandbox()
    {
        Directory.CreateDirectory(Sandbox);
        foreach (var f in new[] { "plugins", "intl", "tzdata", "firebird.msg", "security6.fdb" })
        {
            var link = Path.Combine(Sandbox, f);
            if (new FileInfo(link).LinkTarget == null && !Directory.Exists(link) && !File.Exists(link))
                File.CreateSymbolicLink(link, Path.Combine("/opt/firebird", f));
        }
        File.WriteAllText(Path.Combine(Sandbox, "firebird.conf"), "ServerMode = SuperClassic\n");
    }

    public static void Run(string[] args)
    {
        if (args.Length > 1)
        {
            switch (args[0])
            {
                case "--init": Init(args[1]); break;
                case "--worker": Worker(args[1], args[2]); break;
                case "--check": Check(args[1]); break;
                default: throw new ArgumentException(args[0]);
            }
            return;
        }
        Console.WriteLine("phase 1: two client processes, ONE SuperServer shared cache");
        Phase(ServerCs(), null);

        BuildSandbox();
        Console.WriteLine("phase 2: two EMBEDDED engine processes, PRIVATE page caches");
        Phase(EmbeddedCs(), Sandbox);
        Console.WriteLine("same workload - the private caches paid for coherency in disk I/O.");
    }
}
