//
// Threading.cs - SuperServer's thread-per-attachment topology from the
// outside (C# twin of ../../cpp/threading.cpp; see
// ../../../threading-and-synchronization.md).
//
// The same census: MON$SERVER_PID names the engine process, /proc/<pid>/task
// counts its threads before, during and after twelve concurrent attachments
// from twelve client threads, and MON$ATTACHMENTS shows the Cache Writer
// and Garbage Collector as system attachments.  The workers are twelve
// Tasks started LongRunning (a dedicated thread each), each opening its own
// FbConnection; a CountdownEvent (the Java twin's CountDownLatch) takes the
// "during" census only once all twelve have attached and queried, and a
// ManualResetEventSlim holds them attached until it is taken.  The .NET
// rule is the ADO.NET one: an FbConnection is NOT thread-safe - one
// connection per thread, by documentation rather than by a lock (Jaybird)
// or the compiler (Rust).  The usual way to get "one per thread" cheaply
// is ADO.NET's connection pool, which the sample keeps off (Pooling=false)
// so every Open() is a real attachment and every Dispose() a detach.
// Unlike Jaybird, the provider always fills MON$REMOTE_PROCESS: by default
// isc_dpb_process_name carries the entry assembly's path (.../fbsamples.dll)
// and isc_dpb_process_id the pid; the builder's ApplicationName replaces
// the name, so this attachment names itself.
//
// Run:  cd samples/csharp && dotnet run -- Threading [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Threading
{
    const int Workers = 12;

    static int CountThreads(object? pid) => Directory.GetDirectories($"/proc/{pid}/task").Length;

    public static void Run(string[] args)
    {
        var path = DbPath("threading", args);
        AttachOrCreate(path).Dispose();                  // make sure it exists
        var b = Builder(path);
        b.ApplicationName = "FbSamples.Threading";       // -> isc_dpb_process_name
        using var con = new FbConnection(b.ToString());
        con.Open();

        Execute(con, "recreate table t (id int primary key, v int)");
        Execute(con, "update or insert into t values (1, 0) matching (id)");
        var pid = Scalar(con, "select mon$server_pid from mon$attachments where mon$attachment_id = current_connection");
        Console.WriteLine($"engine process: pid {pid}, {CountThreads(pid)} threads (1 attachment open)");

        using var attached = new CountdownEvent(Workers);
        using var release = new ManualResetEventSlim(false);
        var tasks = new List<Task<object?>>();
        try
        {
            for (var i = 0; i < Workers; i++)
                tasks.Add(Task.Factory.StartNew(() =>
                {
                    using var w = Attach(path);              // one attachment per thread
                    try
                    {
                        return Scalar(w, "select count(*) from t");
                    }
                    finally
                    {
                        attached.Signal();
                        release.Wait();
                    }
                }, TaskCreationOptions.LongRunning));
            attached.Wait();
            // Each query auto-commits: its own transaction, hence a fresh MON$ snapshot.
            Console.WriteLine($"with 12 extra attachments: {CountThreads(pid)} threads | "
                              + Scalar(con, "select count(*) from mon$attachments where mon$system_flag = 0")
                              + " user attachments, "
                              + Scalar(con, "select count(distinct mon$server_pid) from mon$attachments")
                              + " distinct server pid");
        }
        finally
        {
            release.Set();
        }
        Task.WaitAll(tasks.ToArray());                     // surface any worker failure
        Thread.Sleep(1000);
        Console.WriteLine($"after they detach:        {CountThreads(pid)} threads (pooled, not destroyed)");

        Console.WriteLine($"  {"ID",2} {"SYS",3}  {"USER",-18} REMOTE_PROCESS");
        foreach (var r in Rows(con, "select mon$attachment_id, mon$system_flag, trim(mon$user),"
                                    + " coalesce(mon$remote_process, iif(mon$system_flag = 1, '<internal>', '<null>'))"
                                    + " from mon$attachments order by mon$attachment_id"))
            Console.WriteLine($"  {r[0],2} {r[1],3}  {r[2],-18} {r[3]}");
    }
}
