//
// Events.cs - Firebird event notification: the three semantics (C# twin of
// ../../events_demo.cpp; see ../../../firebird-events.md).
//
// A listener registers 'demo_event'; a poster attachment runs PSQL blocks
// with POST_EVENT and shows that (1) a ROLLBACK swallows posts, (2) delivery
// happens at COMMIT, and (3) several posts in one transaction are delivered
// once, with a count.  FirebirdClient implements the auxiliary-channel dance
// in managed code (op_connect_request, a second socket, op_que_events,
// op_event), like Jaybird, node-firebird and the Go driver.  Its listener is
// FbRemoteEvent: built from a connection string, it opens an attachment of
// its own (the Go driver's shape, not Jaybird's createFor(connection)),
// QueueEvents registers the names, and each delivery is raised as the
// RemoteEventCounts .NET event - {Name, Counts} on the provider's event
// thread, handed to the main thread here through a BlockingCollection.  The
// provider consumes the baseline delivery, computes the isc_event_counts
// delta and re-queues the one-shot interest itself.
//
// Run:  cd samples/csharp && dotnet run -- Events [database]
//
using System.Collections.Concurrent;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Events
{
    const string Post = "execute block as begin post_event 'demo_event'; end";

    /// <summary>Deliveries that arrive within the wait: (number of deliveries, total count).</summary>
    static (int Deliveries, int Count) Collect(BlockingCollection<FbRemoteEventCountsEventArgs> q, int waitMs)
    {
        var deliveries = 0;
        var count = 0;
        var timeout = waitMs;
        while (q.TryTake(out var e, timeout))
        {
            deliveries++;
            count += e.Counts;
            timeout = 200;
        }
        return (deliveries, count);
    }

    public static void Run(string[] args)
    {
        var db = args.Length > 0 ? args[0] : "employee";
        var pass = true;
        using var deliveries = new BlockingCollection<FbRemoteEventCountsEventArgs>();

        using var post = Attach(db, "NONE");
        using var listener = new FbRemoteEvent(Builder(db, "NONE").ToString());
        listener.RemoteEventCounts += (_, e) => deliveries.Add(e);
        listener.RemoteEventError += (_, e) => Console.Error.WriteLine("event error: " + e.Error.Message);
        listener.Open();
        listener.QueueEvents(new[] { "demo_event" });
        Console.WriteLine("listener registered for 'demo_event' (baseline consumed by FbRemoteEvent)");

        // 1. ROLLBACK swallows posts.
        using (var tx = post.BeginTransaction())
        {
            Execute(post, Post, tx);
            tx.Rollback();
        }
        var r = Collect(deliveries, 1500);
        Console.WriteLine($"after POST_EVENT + ROLLBACK: delivered count = {r.Count}  "
                          + $"({(r.Count == 0 ? "correct" : "WRONG")} - rollback swallows posts)");
        pass &= r.Count == 0;

        // 2. Delivery is commit-time; 3. posts coalesce into one count.
        using (var tx = post.BeginTransaction())
        {
            for (var i = 0; i < 3; i++)
                Execute(post, Post, tx);
            Console.WriteLine("3 x POST_EVENT executed, not yet committed - waiting briefly...");
            r = Collect(deliveries, 1500);
            Console.WriteLine($"before COMMIT: delivered count = {r.Count}  "
                              + $"({(r.Count == 0 ? "correct" : "WRONG")} - delivery is commit-time)");
            pass &= r.Count == 0;
            tx.Commit();
        }
        r = Collect(deliveries, 5000);
        Console.WriteLine($"after COMMIT: {r.Deliveries} delivery, count = {r.Count}  "
                          + $"({(r.Deliveries == 1 && r.Count == 3 ? "correct" : "WRONG")} - one delivery, count 3)");
        pass &= r.Deliveries == 1 && r.Count == 3;

        listener.CancelEvents();
        Console.WriteLine(pass ? "PASS" : "FAIL");
        if (!pass)
            throw new InvalidOperationException("event semantics not as expected");
    }
}
