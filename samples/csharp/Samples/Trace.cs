//
// Trace.cs - a user trace session driven through the Services API (C# twin
// of ../../cpp/trace.cpp; see ../../../trace-and-audit.md).
//
// The same choreography: service A starts a session whose configuration
// targets one database and streams the TraceLog back; a worker attaches and
// runs one marker query; service B stops the session, which ends A's
// stream.  FirebirdClient has the trace family as a class,
// FirebirdSql.Data.Services.FbTrace, and two things set it apart.  The
// configuration is typed: an FbDatabaseTraceConfiguration with an
// FbDatabaseTraceEvents [Flags] value (Connections | Transactions |
// StatementFinish | PrintPlan | PrintPerf) that the provider renders into
// the isc_spb_trc_cfg text itself (printed below).  And Start(name) is
// SYNCHRONOUS: it sends isc_action_svc_trace_start and then drains the
// stream on the calling thread, raising the ServiceOutput event per line,
// until the session ends - so service A lives on a thread of the sample's
// own, while Stop(id) and List() on fresh FbTrace objects play service B.
// The session id is taken from the stream's first line ("Trace session ID
// n started"), with List() as the fallback, and the stop sits in a finally;
// a last List() checks that no session of this name is left behind.
//
// Run:  cd samples/csharp && dotnet run -- Trace [database]
//
using System.Diagnostics;
using System.Text.RegularExpressions;
using FirebirdSql.Data.Services;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static partial class Trace
{
    const string Name = "hands-on-cs";
    static readonly Stopwatch Clock = Stopwatch.StartNew();

    [GeneratedRegex(@"Trace session ID (\d+) started")]
    private static partial Regex Started();

    static FbTrace Service() => new(FbTraceVersion.Detect, ServiceConnectionString());

    /// <summary>A service B call: its output lines, printed under a prefix.</summary>
    static List<string> Call(string prefix, Action<FbTrace> action)
    {
        var lines = new List<string>();
        var svc = Service();
        svc.ServiceOutput += (_, e) =>
        {
            lines.Add(e.Message);
            if (e.Message.Trim().Length > 0)
                Console.WriteLine($"{prefix} {e.Message}");
        };
        action(svc);
        return lines;
    }

    /// <summary>The id of the named session, from List() output.</summary>
    static int? IdFromList(IEnumerable<string> list, string name)
    {
        int? current = null;
        foreach (var l in list.Select(s => s.Trim()))
        {
            if (l.StartsWith("Session ID:"))
                current = int.Parse(l["Session ID:".Length..].Trim());
            else if (l.StartsWith("name:") && l[5..].Trim() == name)
                return current;
        }
        return null;
    }

    public static void Run(string[] args)
    {
        var path = DbPath("trace", args);
        AttachOrCreate(path).Dispose();                  // the observed database must exist first

        var config = new FbDatabaseTraceConfiguration
        {
            DatabaseName = path,
            Enabled = true,
            Events = FbDatabaseTraceEvents.Connections | FbDatabaseTraceEvents.Transactions
                     | FbDatabaseTraceEvents.StatementFinish | FbDatabaseTraceEvents.PrintPlan
                     | FbDatabaseTraceEvents.PrintPerf,
            TimeThreshold = TimeSpan.Zero,
        };
        Console.WriteLine("[main ] the provider's isc_spb_trc_cfg text:");
        foreach (var l in config.BuildConfiguration(FbTraceVersion.Version2).Split('\n', StringSplitOptions.RemoveEmptyEntries))
            Console.WriteLine("[main ]   " + l.TrimEnd());

        // -- service A: Start() blocks while it drains, so it gets a thread.
        var a = Service();
        a.DatabasesConfigurations.Add(config);
        var gotId = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
        var last = -1000L;
        a.ServiceOutput += (_, e) =>
        {
            var now = Clock.ElapsedMilliseconds;
            if (now - last >= 200)
                Console.WriteLine($"[trace] ---- output arrives at +{now} ms ----");
            last = now;
            if (e.Message.Trim().Length > 0)
                Console.WriteLine($"[trace] {e.Message}");
            if (Started().Match(e.Message) is { Success: true } m)
                gotId.TrySetResult(int.Parse(m.Groups[1].Value));
        };
        Exception? streamError = null;
        var serviceA = new Thread(() =>
        {
            try { a.Start(Name); }
            catch (Exception e) { streamError = e; }
            finally { gotId.TrySetCanceled(); }
        }) { IsBackground = true };
        serviceA.Start();
        Console.WriteLine($"[main ] +{Clock.ElapsedMilliseconds} ms FbTrace.Start(\"{Name}\") running on its own thread");

        int? id = null;
        try
        {
            if (gotId.Task.Wait(TimeSpan.FromSeconds(5)) )
                id = gotId.Task.Result;
            Console.WriteLine($"[main ] +{Clock.ElapsedMilliseconds} ms session id from the stream: {Text(id)}");

            // -- the observed side.
            using (var w = Attach(path))
                Console.WriteLine($"[worker] +{Clock.ElapsedMilliseconds} ms marker query says: "
                                  + Scalar(w, "SELECT COUNT(*) FROM RDB$RELATIONS /* traced! */"));
            Thread.Sleep(1500);
        }
        catch (AggregateException) when (streamError != null)
        {
            // the stream ended early; reported below
        }
        finally
        {
            // -- service B: stop it, by the stream's id or else the server's list.
            id ??= IdFromList(Call("[list ]", s => s.List()), Name);
            if (id is { } sid)
            {
                Console.WriteLine($"[main ] +{Clock.ElapsedMilliseconds} ms FbTrace.Stop({sid})");
                Call("[stop ]", s => s.Stop(sid));
            }
            if (!serviceA.Join(TimeSpan.FromSeconds(10)))
                Console.WriteLine("[main ] WARNING: service A's stream did not end");
        }
        if (streamError != null)
            throw streamError;

        var left = IdFromList(Call("[check]", s => s.List()), Name);
        Console.WriteLine($"[main ] sessions named \"{Name}\" left on the server: {(left is null ? "none" : left)}");
        Console.WriteLine("done.");
    }
}
