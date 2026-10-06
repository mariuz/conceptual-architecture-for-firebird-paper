//
// ParallelWorkers.cs - watching worker attachments appear, and be refused
// (C# twin of ../../cpp/parallel_workers.cpp; see ../../../parallel-workers.md).
//
// [A] Against the live server, over the provider's managed wire protocol:
//     ask for 4 workers with the connection-string key ParallelWorkers
//     (FirebirdClient's name for isc_dpb_parallel_workers).  Both knobs are
//     GLOBAL config, so with the stock MaxParallelWorkers = 1 the engine
//     clamps the request and attaches with a status-vector WARNING.  ADO.NET's
//     channel for warnings is the connection's InfoMessage event, but the
//     provider does not route the attach response's warning there - it is
//     lost, as in fb-cpp and fbintf - so the clamp is read back from SQL,
//     as MON$ATTACHMENTS.MON$PARALLEL_WORKERS.
// [B] Against an embedded engine (ServerType=Embedded: the provider loads
//     libfbclient, and with it the engine, into this process) whose private
//     FIREBIRD root sets ParallelWorkers = 4 / MaxParallelWorkers = 8: 200k
//     incompressible rows, CREATE INDEX, and a second embedded attachment
//     polling MON$ATTACHMENTS from a Task.  The engine reads FIREBIRD with
//     getenv(), and .NET's Environment.SetEnvironmentVariable on Unix only
//     changes the runtime's managed copy of the environment - so the sample
//     P/Invokes libc's setenv before the engine loads, the C++ sample's
//     setenv one interop hop away (Jaybird needs JNA for the same step).
//
// Run:  cd samples/csharp && dotnet run -- ParallelWorkers   (~1 min)
//
using System.Diagnostics;
using System.Runtime.InteropServices;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class ParallelWorkers
{
    const string Root = Scratch + "/fbroot-parallel-cs";

    [DllImport("libc", EntryPoint = "setenv")]
    static extern int SetEnv(string name, string value, int overwrite);

    /// <summary>A private $FIREBIRD root: symlinks into the stock install, own firebird.conf.</summary>
    static void MakeRoot()
    {
        Directory.CreateDirectory(Root);
        foreach (var f in new[] { "plugins", "intl", "firebird.msg", "tzdata", "plugins.conf", "databases.conf" })
        {
            var link = Path.Combine(Root, f);
            if (new FileInfo(link).LinkTarget == null)
                File.CreateSymbolicLink(link, Path.Combine("/opt/firebird", f));
        }
        File.WriteAllText(Path.Combine(Root, "firebird.conf"),
                          "ServerMode = Super\nParallelWorkers = 4\nMaxParallelWorkers = 8\n");
    }

    static string Knobs(FbConnection con) => string.Join(", ",
        Rows(con, "select rdb$config_name, rdb$config_value from rdb$config "
                + "where rdb$config_name in ('ParallelWorkers', 'MaxParallelWorkers') "
                + "order by rdb$config_name desc")
            .Select(r => $"{Text(r[0]).Trim()} = {Text(r[1])}"));

    static FbConnection Embedded(string path)
    {
        var b = Builder(path);
        b.ServerType = FbServerType.Embedded;
        b.ClientLibrary = "/opt/firebird/lib/libfbclient.so";
        var cs = b.ToString();
        if (!File.Exists(path))
            FbConnection.CreateDatabase(cs, overwrite: false);
        var con = new FbConnection(cs);
        con.Open();
        return con;
    }

    public static void Run(string[] args)
    {
        var serverDb = DbPath("parallel", args);
        var embeddedDb = args.Length > 1 ? args[1] : Scratch + "/par_embedded_cs.fdb";

        // --- [A] the server refuses politely --------------------------------
        AttachOrCreate(serverDb).Dispose();
        var b = Builder(serverDb);
        b.ParallelWorkers = 4;
        using (var con = new FbConnection(b.ToString()))
        {
            var warnings = new List<string>();
            con.InfoMessage += (_, e) => warnings.Add(e.Message);
            con.Open();
            Console.WriteLine("[A] server attach, ParallelWorkers=4 (isc_dpb_parallel_workers)");
            foreach (var w in warnings)
                Console.WriteLine("    InfoMessage: " + w.Trim());
            if (warnings.Count == 0)
                Console.WriteLine("    InfoMessage: (none raised for the attach)");
            Console.WriteLine($"    server config: {Knobs(con)}; granted MON$PARALLEL_WORKERS = "
                              + Scalar(con, "select mon$parallel_workers from mon$attachments "
                                          + "where mon$attachment_id = current_connection")
                              + " -> 0 extra workers\n");
        }

        // --- [B] embedded engine with its own firebird.conf -----------------
        MakeRoot();
        SetEnv("FIREBIRD", Root, 1);        // before libfbclient (and the engine) load
        using var main = Embedded(embeddedDb);
        Console.WriteLine($"[B] embedded attach, FIREBIRD={Root}");
        Console.WriteLine($"    engine config: {Knobs(main)}");
        Execute(main, "recreate table parade (id int, val varchar(200))");
        // Incompressible filler: the index build goes parallel only when the
        // relation spans more than one pointer page.
        Execute(main, "execute block as declare n int = 0; begin "
                    + "  while (n < 200000) do begin "
                    + "    insert into parade values (:n, "
                    + "      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
                    + "      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
                    + "      uuid_to_char(gen_uuid())); "
                    + "    n = n + 1; "
                    + "  end end");
        Console.WriteLine("    parade table: 200000 rows of 180 incompressible bytes, "
                          + Scalar(main, "select count(*) from rdb$pages p join rdb$relations r "
                                       + "  on p.rdb$relation_id = r.rdb$relation_id "
                                       + "where r.rdb$relation_name = 'PARADE' and p.rdb$page_type = 4")
                          + " pointer pages");

        var maxSeen = 0;
        var roster = "";
        using var stop = new CancellationTokenSource();
        var poller = Task.Run(() =>
        {
            using var mon = Embedded(embeddedDb);   // auto-commit: a fresh MON$ snapshot per query
            while (!stop.IsCancellationRequested)
            {
                var n = Convert.ToInt32(Scalar(mon, "select count(*) from mon$attachments where mon$user = '<Worker>'"));
                if (n > maxSeen)
                {
                    maxSeen = n;
                    roster = string.Concat(Rows(mon, "select trim(mon$user), mon$system_flag "
                                                   + "from mon$attachments order by mon$attachment_id")
                        .Select(r => $"        {r[0]}  (system_flag {r[1]})\n"));
                }
                Thread.Sleep(20);
            }
        });

        var sw = Stopwatch.StartNew();
        Execute(main, "create index ix_parade on parade (val)");
        sw.Stop();
        stop.Cancel();
        poller.Wait();
        Console.WriteLine($"    create index: {sw.ElapsedMilliseconds} ms; max '<Worker>' attachments seen: {maxSeen}");
        Console.Write("    MON$ATTACHMENTS at the widest moment:\n" + roster);
        Console.WriteLine("    after build: workers stay pooled (idle timeout 60 s): "
                          + Scalar(main, "select count(*) from mon$attachments where mon$user = '<Worker>'"));
        Console.WriteLine("done.");
    }
}
