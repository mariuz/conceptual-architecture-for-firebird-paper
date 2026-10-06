//
// Services.cs - the Services API from C#: a service attachment, an
// information request, and a verbose server-side backup (C# twin of
// ../../cpp/services.cpp; see ../../../services-api.md).
//
// FirebirdClient speaks the Services protocol itself on its managed wire
// (op_service_attach / op_service_start / op_service_info), so, as with
// the Go, JavaScript and Java drivers, "localhost" can only mean the remote
// service_mgr.  FirebirdSql.Data.Services wraps it in one class per action:
// FbServerProperties.GetServerVersion() is the isc_info_svc_server_version
// request, and FbBackup.Execute() starts isc_action_svc_backup with SERVER
// paths (Database in the connection string, BackupFiles for the .fbk) and
// then drains the output on the calling thread, raising ServiceOutput per
// line.  The drain is the C++ sample's loop: each line is its own
// isc_info_svc_line query (FbService.GetNextLine), so the verbose lines
// cost one op_service_info round trip each, plus the empty answer that
// ends the stream - the opposite end from Jaybird's isc_info_svc_to_eof
// chunks.  The sample times the stream to show it.
//
// Run:  cd samples/csharp && dotnet run -- Services [database [backup]]
//
using System.Diagnostics;
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Services;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Services
{
    static string Mode(UnixFileMode m) => string.Concat(
        new[] { UnixFileMode.UserRead, UnixFileMode.UserWrite, UnixFileMode.UserExecute,
                UnixFileMode.GroupRead, UnixFileMode.GroupWrite, UnixFileMode.GroupExecute,
                UnixFileMode.OtherRead, UnixFileMode.OtherWrite, UnixFileMode.OtherExecute }
            .Select((bit, i) => (m & bit) != 0 ? "rwx"[i % 3] : '-'));

    public static void Run(string[] args)
    {
        var db = DbPath("services", args);
        var fbk = args.Length > 1 ? args[1] : Path.ChangeExtension(db, ".fbk");

        // 0. The scratch database (idempotent).
        using (var con = AttachOrCreate(db))
            try { Execute(con, "create table t (id int, v varchar(20))"); } catch (FbException) { /* fine */ }

        // 1+2. Service attachment + information request.
        var props = new FbServerProperties(ServiceConnectionString());
        Console.WriteLine($"service       : {Host}:service_mgr (managed wire)");
        Console.WriteLine($"server version: {props.GetServerVersion()}");

        // 3+4. The backup action; Execute() drains the output line by line.
        var backup = new FbBackup(ServiceConnectionString(db)) { Verbose = true };   // database: server path!
        backup.BackupFiles.Add(new FbBackupFile(fbk, null));                        // server path!
        var lines = new List<string>();
        var sw = new Stopwatch();
        backup.ServiceOutput += (_, e) => lines.Add(e.Message.TrimEnd());
        Console.WriteLine("backup started (verbose) - FbBackup drains with isc_info_svc_line:");
        sw.Start();
        backup.Execute();
        sw.Stop();

        var n = lines.Count;
        for (var i = 0; i < n; i++)
        {
            if (i < 3 || i == n - 1)
                Console.WriteLine("  " + lines[i]);
            else if (i == 3)
                Console.WriteLine("  ...");
        }
        Console.WriteLine($"done: {n} gbak lines = {n} ServiceOutput events = {n} isc_info_svc_line round trips"
                          + $" (+1 empty) in {sw.ElapsedMilliseconds} ms");

        var file = new FileInfo(fbk);
        if (file.Directory is { Exists: true } && file.Exists)
            Console.WriteLine($"the file {fbk} now exists on the SERVER: {file.Length} bytes, mode {Mode(file.UnixFileMode)}"
                              + " (written by the server's user, not by this client)");
    }
}
