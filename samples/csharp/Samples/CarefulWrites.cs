//
// CarefulWrites.cs - kill a database engine mid-write; the file needs no
// recovery (C# twin of ../../cpp/careful_writes.cpp; see
// ../../../careful-writes-and-crash-safety.md).
//
// Like the C++ twin this uses the EMBEDDED engine, so the process that is
// killed IS the engine: ServerType=Embedded makes FirebirdClient P/Invoke
// libfbclient, and a plain local path makes its Y-valve load the Engine
// provider into the .NET process.  .NET has no fork(), so the parent
// starts a second copy of this program with --writer (Process.Start on
// Environment.ProcessPath, FIREBIRD=/opt/firebird in its environment).  The
// writer creates the database, commits a marker row, then bulk-inserts
// 500,000 rows in a transaction it never commits.  The parent watches the
// .fdb grow (the engine flushing pages of the UNCOMMITTED transaction),
// sends SIGKILL with Process.Kill(), re-attaches embedded and counts:
// committed rows all present, uncommitted rows all gone, no log replay -
// because there is no log.
//
// Run:  cd samples/csharp && dotnet run -- CarefulWrites [path]
//
using System.Diagnostics;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class CarefulWrites
{
    static string Embedded(string path)
    {
        var b = Builder(path);
        b.ServerType = FbServerType.Embedded;
        b.ClientLibrary = "/opt/firebird/lib/libfbclient.so";
        return b.ToString();
    }

    /// <summary>Child mode: a process that is the engine; commits a marker, then never commits.</summary>
    static void Writer(string path)
    {
        FbConnection.CreateDatabase(Embedded(path), overwrite: true);
        using var con = new FbConnection(Embedded(path));
        con.Open();
        Execute(con, "create table cw (id int, tag varchar(30))");
        Execute(con, "insert into cw values (1, 'committed-marker')");
        Console.WriteLine($"[writer {Environment.ProcessId}] marker row committed (embedded engine in this process)");

        var victim = con.BeginTransaction();   // the crash victim: never committed
        Execute(con,
            "execute block as declare i int = 0; begin" +
            "  while (i < 500000) do begin" +
            "    insert into cw values (:i + 1000, 'uncommitted'); i = i + 1;" +
            "  end " +
            "end", victim);
        Console.WriteLine("[writer] bulk insert finished uncommitted; waiting for SIGKILL");
        Thread.Sleep(Timeout.Infinite);
    }

    public static void Run(string[] args)
    {
        if (args.Length > 0 && args[0] == "--writer")
        {
            Writer(args[1]);
            return;
        }
        var path = args.Length > 0 ? args[0] : $"{Scratch}/careful_writes_cs.fdb";
        File.Delete(path);                       // fresh run

        // 1. Spawn the writer: a separate process running the embedded engine.
        var self = Environment.ProcessPath!;
        var psi = new ProcessStartInfo(self) { UseShellExecute = false };
        if (Path.GetFileNameWithoutExtension(self) == "dotnet")      // started as "dotnet FbSamples.dll"
            psi.ArgumentList.Add(typeof(CarefulWrites).Assembly.Location);
        foreach (var a in new[] { nameof(CarefulWrites), "--writer", path })
            psi.ArgumentList.Add(a);
        psi.Environment["FIREBIRD"] = "/opt/firebird";
        using var writer = Process.Start(psi)!;

        // 2. Wait until the file is visibly growing, then kill -9 the engine.
        long baseSize = -1;
        for (var i = 0; i < 600; i++)
        {
            Thread.Sleep(50);
            var size = File.Exists(path) ? new FileInfo(path).Length : 0;
            if (baseSize < 0 && size > 0) baseSize = size;
            if (baseSize > 0 && size > baseSize + 2 * 1024 * 1024)
            {
                Console.WriteLine($"file grew {baseSize} -> {size} bytes; SIGKILL to engine pid {writer.Id}");
                break;
            }
        }
        writer.Kill();                           // SIGKILL on Unix
        writer.WaitForExit();

        // 3. Re-attach immediately: there is no recovery step to run.
        var sw = Stopwatch.StartNew();
        string committed, uncommitted;
        using (var con = new FbConnection(Embedded(path)))
        {
            con.Open();
            committed = Text(Scalar(con, "select count(*) from cw where tag = 'committed-marker'"));
            uncommitted = Text(Scalar(con, "select count(*) from cw where tag = 'uncommitted'"));
        }
        Console.WriteLine($"re-attach + both counts took {sw.ElapsedMilliseconds} ms");
        Console.WriteLine($"committed marker rows : {committed}   <- survived the crash");
        Console.WriteLine($"uncommitted rows      : {uncommitted}   <- rolled back by visibility, not replay");
        Console.WriteLine($"({new FileInfo(path).Length} bytes on disk after the crash)");
    }
}
