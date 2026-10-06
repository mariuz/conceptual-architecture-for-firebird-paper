//
// Backup.cs - a gbak backup + restore round trip through the Services API
// (C# twin of ../../cpp/backup.cpp; see ../../../backup-and-recovery.md).
//
//   1. create a scratch database with a table and three rows;
//   2. FbBackup: isc_action_svc_backup (verbose) to a server-side .fbk
//      while the source attachment stays open (gbak reads through a
//      snapshot - "online backup");
//   3. FbRestore: isc_action_svc_restore (replace), verbose;
//   4. attach to the restored copy and prove the rows survived;
//   5. FbStreamingBackup / FbStreamingRestore: the same services with the
//      backup file set to "stdout" / "stdin", so the .fbk bytes travel over
//      the service connection into this process (a MemoryStream here; any
//      System.IO.Stream - a FileStream, a GZipStream, a socket) and back.
//
// FirebirdClient's managed client speaks the Services API itself (no gbak
// binary, no libfbclient): the FbService classes build the SPB from
// properties and run the isc_info_svc_line drain loop, raising one
// ServiceOutput event per line of gbak's log.
//
// Run:  cd samples/csharp && dotnet run -- Backup [database]
//
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Services;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Backup
{
    static int Count(string db) { using var c = Attach(db); return Convert.ToInt32(Scalar(c, "select count(*) from br_items")); }
    static string MaxName(string db) { using var c = Attach(db); return Text(Scalar(c, "select max(name) from br_items")); }

    public static void Run(string[] args)
    {
        var src = DbPath("backup", args);
        var fbk = Path.ChangeExtension(src, ".fbk");                     // a server-side path
        var restored = src.Replace(".fdb", "_restored.fdb");
        var streamed = src.Replace(".fdb", "_streamed.fdb");

        using var source = AttachOrCreate(src);
        Execute(source, "recreate table br_items (id integer primary key, name varchar(20))");
        Execute(source, "insert into br_items select 1, 'alpha' from rdb$database union all " +
                        "select 2, 'beta' from rdb$database union all select 3, 'gamma' from rdb$database");
        Console.WriteLine("source ready: BR_ITEMS with 3 rows (attachment kept open)");

        // -- 2. backup to a server-side file ------------------------------------
        Console.WriteLine($"\n== FbBackup: {src} -> {fbk} ==");
        var backup = new FbBackup(ServiceConnectionString(src)) { Verbose = true };
        backup.BackupFiles.Add(new FbBackupFile(fbk, null));
        backup.ServiceOutput += (_, e) => Console.WriteLine("  [backup] " + e.Message);
        backup.Execute();

        // -- 3. restore it ------------------------------------------------------
        Console.WriteLine($"\n== FbRestore: {fbk} -> {restored} ==");
        var restore = new FbRestore(ServiceConnectionString(restored))
        {
            Verbose = true,
            Options = FbRestoreFlags.Create | FbRestoreFlags.Replace,
        };
        restore.BackupFiles.Add(new FbBackupFile(fbk, null));
        restore.ServiceOutput += (_, e) => Console.WriteLine("  [restore] " + e.Message);
        restore.Execute();

        // -- 4. the rows survived -----------------------------------------------
        Console.WriteLine($"\nrestored database says: {Count(restored)} rows, max name = {MaxName(restored)}");

        // -- 5. streaming: the .fbk never touches the server's disk -------------
        Console.WriteLine("\n== FbStreamingBackup -> MemoryStream -> FbStreamingRestore ==");
        using var fbkBytes = new MemoryStream();
        new FbStreamingBackup(ServiceConnectionString(src)) { OutputStream = fbkBytes }.Execute();
        Console.WriteLine($"received {fbkBytes.Length} bytes of backup into a .NET MemoryStream");

        fbkBytes.Position = 0;
        var lines = 0;
        var streamingRestore = new FbStreamingRestore(ServiceConnectionString(streamed))
        {
            InputStream = fbkBytes,
            Verbose = true,
            Options = FbRestoreFlags.Create | FbRestoreFlags.Replace,
        };
        streamingRestore.ServiceOutput += (_, _) => lines++;
        streamingRestore.Execute();
        Console.WriteLine($"restored from the stream ({lines} verbose lines), {streamed} says: " +
                          $"{Count(streamed)} rows, max name = {MaxName(streamed)}");
        Console.WriteLine("done.");
    }
}
