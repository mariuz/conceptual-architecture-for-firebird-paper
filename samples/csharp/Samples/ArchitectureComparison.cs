//
// ArchitectureComparison.cs - one provider, three ways into the engine (C#
// twin of ../../cpp/architecture_comparison.cpp; see
// ../../../architecture-comparison.md).
//
// The C++ twin attaches twice through libfbclient: inet://localhost/employee
// goes to the Y-valve's Remote provider, a bare local path loads the Engine
// provider INTO the process.  FirebirdClient holds both client families in
// one assembly, chosen by the connection string's ServerType:
//
//   1. ServerType=Default  - the provider's own managed (pure C#)
//      implementation of the wire protocol, no libfbclient at all;
//   2. ServerType=Embedded with Database=[::1]:employee - the provider
//      P/Invokes libfbclient (ClientLibrary) and the Y-valve's Remote
//      provider does the talking, as in the C++ sample;
//   3. ServerType=Embedded with a bare local path - the same libfbclient,
//      and the Y-valve loads the Engine provider into this .NET process.
//
// Leg 2 took two detours, both instructive.  (a) The connection-string
// parser splits "inet://host/db", "host:db" and "host/port:db" into
// DataSource + Database, and ServerType=Embedded then passes ONLY Database
// to isc_attach_database: a first version with inet://localhost/employee
// got an in-process engine that opened employee itself (NETWORK_PROTOCOL
// NULL, our own pid).  "[::1]:employee" is a host form the parser does not
// recognise, so it reaches libfbclient intact.  (b) The native path never
// puts isc_dpb_password in the DPB (the managed client sends it in its
// auth block), so a remote attach through libfbclient fails with "Your
// user name and password are not defined" unless libfbclient finds
// ISC_USER / ISC_PASSWORD in the C environment - which .NET's
// Environment.SetEnvironmentVariable does not touch on Linux, hence the
// setenv P/Invoke below.
//
// Each attachment answers the C++ sample's three questions (ENGINE_VERSION,
// NETWORK_PROTOCOL, MON$SERVER_PID vs our own pid) plus the
// isc_info_firebird_version text (FbDatabaseInfo.GetServerVersion) and the
// negotiated protocol (GetProtocolVersion).
//
// Run:  cd samples/csharp && dotnet run -- ArchitectureComparison [remote-db] [local-path]
//
using System.Runtime.InteropServices;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class ArchitectureComparison
{
    [DllImport("libc", EntryPoint = "setenv")]
    static extern int SetEnv(string name, string value, int overwrite);

    const string ClientLibrary = "/opt/firebird/lib/libfbclient.so";

    static void Inspect(string label, FbConnectionStringBuilder b, bool create)
    {
        if (create && !File.Exists(b.Database))
            FbConnection.CreateDatabase(b.ToString(), overwrite: false);

        using var con = new FbConnection(b.ToString());
        con.Open();
        var row = Rows(con,
            "select rdb$get_context('SYSTEM', 'ENGINE_VERSION'), " +
            "       rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), " +
            "       a.mon$server_pid " +
            "from mon$attachments a " +
            "where a.mon$attachment_id = current_connection")[0];
        var serverPid = Convert.ToInt32(row[2]);
        var pid = Environment.ProcessId;
        var info = new FbDatabaseInfo(con);

        Console.WriteLine(label);
        Console.WriteLine(b.ServerType == FbServerType.Default
            ? $"    connection        : ServerType=Default, DataSource={b.DataSource}, Database={b.Database}"
            : $"    connection        : ServerType=Embedded, Database={b.Database} (handed to libfbclient)");
        Console.WriteLine($"    ENGINE_VERSION    : {Text(row[0])}");
        Console.WriteLine($"    NETWORK_PROTOCOL  : {Text(row[1])}");
        Console.WriteLine($"    MON$SERVER_PID    : {serverPid}   (this process is pid {pid}"
                          + (serverPid == pid ? " -- the engine runs IN this process)" : ")"));
        Console.WriteLine($"    info version      : {info.GetServerVersion()}");
        Console.WriteLine($"    protocol version  : {info.GetProtocolVersion()}");
    }

    public static void Run(string[] args)
    {
        var remote = args.Length > 0 ? args[0] : "employee";
        var local = args.Length > 1 ? args[1] : $"{Scratch}/arch_embedded_cs.fdb";

        Console.WriteLine("One ADO.NET provider: its own wire client, then one libfbclient with two providers.");
        Console.WriteLine();

        var managed = Builder(remote, "NONE");
        Inspect("[1] ServerType=Default (the provider's managed wire protocol):", managed, false);
        Console.WriteLine();

        SetEnv("ISC_USER", User, 1);           // libfbclient's own credentials
        SetEnv("ISC_PASSWORD", Password, 1);
        var viaClient = Builder($"[::1]:{remote}", "NONE");
        viaClient.ServerType = FbServerType.Embedded;
        viaClient.ClientLibrary = ClientLibrary;
        Inspect("[2] ServerType=Embedded, [::1]:employee -> libfbclient's Remote provider:", viaClient, false);
        Console.WriteLine();

        var embedded = Builder(local, "NONE");
        embedded.ServerType = FbServerType.Embedded;
        embedded.ClientLibrary = ClientLibrary;
        Inspect("[3] ServerType=Embedded, local path -> libfbclient's Engine provider:", embedded, true);
        Console.WriteLine();
        Console.WriteLine("done.");
    }
}
