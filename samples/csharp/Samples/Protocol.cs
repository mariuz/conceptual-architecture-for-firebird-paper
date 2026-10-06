//
// Protocol.cs - the negotiated wire session, from both ends (C# twin of
// ../../protocol_client.cpp, ../../java/src/main/java/fbsamples/Protocol.java
// and ../../go/protocol/main.go; see ../../../firebird-wire-protocol.md).
//
// FirebirdClient's default ServerType is an independent C# implementation
// of the wire document: op_connect with a version list (up to protocol 16
// in 10.3.4 - its newest Client/Managed/VersionNN), op_cond_accept / op_accept_data,
// Srp256 / Srp / Legacy_Auth, op_crypt with Arc4 (the only cipher it
// implements), and zlib wire compression - no libfbclient.  The sample
// attaches, prints the C++ sample's SYSTEM-context answers and the
// MON$ATTACHMENTS row the server recorded, then the client's own view
// through FbDatabaseInfo (isc_info_firebird_version, fb_info_protocol_version,
// fb_info_wire_crypt).
//
// Because the handshake is the provider's code, connection-string keys steer
// it: Compression=true adds zlib on top of the cipher (the engine records
// it), WireCrypt=Required / Disabled are the client half of the server's
// WireCrypt setting.  There is no key to choose the auth plugin or the
// cipher.
//
// Run:  cd samples/csharp && dotnet run -- Protocol [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Protocol
{
    static string Ctx(FbConnection con, string name) =>
        Scalar(con, $"select rdb$get_context('SYSTEM', '{name}') from rdb$database") is { } v ? Text(v) : "(none)";

    static void Session(string title, string database, Action<FbConnectionStringBuilder> tweak)
    {
        Console.WriteLine($"== {title} ==");
        var b = Builder(database, "NONE");      // stock employee.fdb is charset NONE
        tweak(b);
        try
        {
            using var con = new FbConnection(b.ToString());
            con.Open();
            Console.WriteLine($"attached to {b.DataSource}:{b.Database}  (WireCrypt={b.WireCrypt}, Compression={b.Compression})");
            Console.WriteLine($"engine version : {Ctx(con, "ENGINE_VERSION")}");
            Console.WriteLine($"protocol       : {Ctx(con, "NETWORK_PROTOCOL")}");
            Console.WriteLine($"wire crypt     : {Ctx(con, "WIRE_CRYPT_PLUGIN")}");
            Console.WriteLine($"authenticated  : {Text(Scalar(con, "select trim(current_user) from rdb$database"))}");
            var a = Rows(con, "select mon$auth_method, mon$remote_version, mon$wire_crypt_plugin," +
                              " mon$wire_compressed, mon$client_version" +
                              " from mon$attachments where mon$attachment_id = current_connection")[0];
            Console.WriteLine("MON$ATTACHMENTS, as the server recorded the handshake:");
            Console.WriteLine($"   auth method    : {Text(a[0])}");
            Console.WriteLine($"   wire protocol  : {Text(a[1])}");
            Console.WriteLine($"   wire crypt     : {Text(a[2])}");
            Console.WriteLine($"   compressed     : {Text(a[3])}");
            Console.WriteLine($"   client version : {Text(a[4])}");
            var info = new FbDatabaseInfo(con);
            Console.WriteLine("the client side (FbDatabaseInfo):");
            Console.WriteLine($"   server version : {info.GetServerVersion()}");
            Console.WriteLine($"   -> protocol {info.GetProtocolVersion()}, wire crypt {info.GetWireCrypt()}");
        }
        catch (FbException e)
        {
            Console.WriteLine($"attach refused : {ErrorText(e).Replace("\n", "; ")} [SQLSTATE {e.SQLSTATE}, gds {e.ErrorCode}]");
        }
        Console.WriteLine();
    }

    public static void Run(string[] args)
    {
        var database = args.Length > 0 ? args[0] : "employee";
        Session("defaults: WireCrypt=Enabled, the provider's preferences", database, _ => { });
        Session("Compression=true: zlib on top of the cipher", database, b => b.Compression = true);
        Session("WireCrypt=Required: the client insists", database, b => b.WireCrypt = FbWireCrypt.Required);
        Session("WireCrypt=Disabled: the client refuses to encrypt", database, b => b.WireCrypt = FbWireCrypt.Disabled);
        Console.WriteLine("detached. bye");
    }
}
