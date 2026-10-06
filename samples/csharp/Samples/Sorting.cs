//
// Sorting.cs - the TempCacheLimit threshold made visible (C# twin of
// ../../cpp/sorting.cpp; see ../../../sorting-and-temp-space.md).
//
// The same experiment: 200,000 rows with a 400-byte ASCII key (~82 MB of
// sort data) sorted whole, against a 20,000-row (~8 MB) variant of the same
// ORDER BY.  While each query runs, a watcher on a thread of its own, with
// its OWN attachment, polls database-level MON$MEMORY_USAGE (a fresh
// transaction per poll - a fresh MON$ snapshot) and looks for the unlinked
// fb_sort_* scratch files.  FirebirdClient's managed wire protocol blocks
// only the thread waiting on its socket, so the watcher keeps polling while
// the main thread sits in the Read() that performs the sort.  The plan
// comes from FbCommand in both forms: GetCommandPlan() gives the legacy
// PLAN SORT (...), GetCommandExplainedPlan() the tree whose Sort node
// carries the record and key lengths.  The scratch half has the same
// privilege gap as the Python, Go and Java twins: the files exist only in
// the server's /proc/<pid>/fd, which needs the server's uid or root, so
// the fallback watches the free space of the scratch filesystem (/tmp, the
// default TempDirectories; FIREBIRD_TMP overrides) through DriveInfo - an
// unlinked file still occupies its blocks.
//
// Run:  cd samples/csharp && dotnet run -- Sorting [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Sorting
{
    const string MemSql = "select m.mon$memory_allocated from mon$database d"
                          + " join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id";
    static readonly string ScratchFs = Environment.GetEnvironmentVariable("FIREBIRD_TMP") is { Length: > 0 } t ? t : "/tmp";

    /// <summary>Polls MON$ and the scratch side until stopped, keeping the peaks.</summary>
    sealed class Watcher
    {
        volatile bool running = true;
        public long PeakMem, PeakFiles, PeakScratch, PeakDrop;
        public bool ProcVisible = true;
        public Exception? Failure;
        readonly string path;
        readonly long pid;
        readonly long freeBefore;
        readonly Thread thread;

        public Watcher(string path, long pid)
        {
            this.path = path;
            this.pid = pid;
            freeBefore = new DriveInfo(ScratchFs).AvailableFreeSpace;
            thread = new Thread(Loop) { IsBackground = true };
            thread.Start();
        }

        public void Stop()
        {
            running = false;
            thread.Join();
        }

        /// <summary>Count and sizes of the server's open fb_sort_* files, or null when /proc is closed to us.</summary>
        (long files, long bytes)? ViaProc()
        {
            try
            {
                long files = 0, bytes = 0;
                foreach (var fd in Directory.EnumerateFiles($"/proc/{pid}/fd"))
                {
                    var info = new FileInfo(fd);
                    if (info.LinkTarget?.Contains("fb_sort") == true)
                    {
                        files++;
                        using var h = File.OpenHandle(fd);       // opens the (unlinked) file itself
                        bytes += RandomAccess.GetLength(h);
                    }
                }
                return (files, bytes);
            }
            catch (Exception e) when (e is UnauthorizedAccessException or IOException)
            {
                return null;
            }
        }

        void Loop()
        {
            try
            {
                using var mon = Attach(path);
                while (running)
                {
                    var seen = ProcVisible ? ViaProc() : null;
                    if (seen is { } s)
                    {
                        PeakFiles = Math.Max(PeakFiles, s.files);
                        PeakScratch = Math.Max(PeakScratch, s.bytes);
                    }
                    else
                    {
                        ProcVisible = false;
                        PeakDrop = Math.Max(PeakDrop, freeBefore - new DriveInfo(ScratchFs).AvailableFreeSpace);
                    }
                    using var tx = mon.BeginTransaction();      // new tx => new MON$ snapshot
                    PeakMem = Math.Max(PeakMem, Convert.ToInt64(Scalar(mon, MemSql, tx)));
                    tx.Commit();
                }
            }
            catch (Exception e)
            {
                Failure = e;
            }
        }
    }

    public static void Run(string[] args)
    {
        var path = DbPath("sorting", args);
        using var con = AttachOrCreate(path);
        Execute(con, "recreate table bulk (id integer, pad varchar(400) character set ascii)");
        Execute(con, "execute block as declare i integer = 0; begin"
                     + "  while (i < 200000) do begin"
                     + "    insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x'));"
                     + "    i = i + 1;"
                     + "  end "
                     + "end");
        Console.WriteLine("bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data");

        var pid = Convert.ToInt64(Scalar(con, "select mon$server_pid from mon$attachments"
                                              + " where mon$attachment_id = current_connection"));
        var memIdle = Convert.ToInt64(Scalar(con, MemSql));
        Console.WriteLine($"server pid {pid}, database memory allocated while idle: {memIdle} bytes");

        (string label, string sql)[] cases =
        {
            ("big sort (200k rows, ~82 MB)", "select first 1 id from bulk order by pad desc"),
            ("small sort (20k rows, ~8 MB)", "select first 1 id from bulk where mod(id, 10) = 0 order by pad desc"),
        };
        foreach (var (label, sql) in cases)
        {
            object? top;
            var w = new Watcher(path, pid);
            try
            {
                using var tx = con.BeginTransaction();
                using var cmd = new FbCommand(sql, con, tx);
                cmd.Prepare();
                Console.WriteLine();
                Console.WriteLine(label);
                Console.WriteLine("  " + cmd.GetCommandPlan().Trim());
                foreach (var line in cmd.GetCommandExplainedPlan().Split('\n').Where(l => l.Contains("Sort")))
                    Console.WriteLine("  " + line.Trim());
                using (var r = cmd.ExecuteReader())             // the sort happens in the first Read()
                    top = r.Read() ? r.GetValue(0) : null;
                tx.Commit();
            }
            finally
            {
                w.Stop();
            }
            if (w.Failure != null)
                throw w.Failure;
            Console.WriteLine($"  top row id = {top}");
            if (w.ProcVisible)
                Console.WriteLine($"  peak fb_sort_* scratch: {w.PeakFiles} file(s), {w.PeakScratch} bytes");
            else
                Console.WriteLine($"  /proc/{pid}/fd not readable; peak drop of free space on {ScratchFs}: {Math.Max(0, w.PeakDrop)} bytes");
            Console.WriteLine($"  peak database MON$MEMORY_ALLOCATED: {w.PeakMem} bytes (+{w.PeakMem - memIdle} over idle)");
        }
        Console.WriteLine();
        Console.WriteLine("done.");
    }
}
