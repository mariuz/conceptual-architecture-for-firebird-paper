//
// ApiStyles.cs - one query through the API levels of one .NET provider (C#
// twin of ../../cpp/api_styles.cpp; see ../../../client-apis-and-drivers.md).
//
// The C++ sample runs one SELECT through both C APIs of libfbclient (the
// legacy ISC API and the OO API).  FirebirdClient is the .NET row of the
// driver table, and like Jaybird it holds both strategies:
//
//   1. portable ADO.NET - DbProviderFactory / DbConnection / DbCommand, no
//      Firebird type in sight (FirebirdClientFactory.Instance);
//   2. the provider's own types on the same managed wire client: FbCommand,
//      a typed FbDataReader, FbDatabaseInfo (isc_info_* items) - and,
//      underneath, an internal GdsDatabase class per protocol version
//      (named here through reflection);
//   3. ServerType=Embedded - the same ADO.NET calls go to FesDatabase,
//      which P/Invokes libfbclient through the LEGACY ISC API
//      (isc_attach_database, isc_dsql_prepare, XSQLDA ... - the public
//      FirebirdSql.Data.Client.Native.IFbClient interface lists them), the
//      calls of the C++ sample's first half.  Database = "[::1]:employee"
//      because the connection-string parser would split "inet://..." and
//      the embedded path drops DataSource; credentials go to libfbclient as
//      ISC_USER / ISC_PASSWORD, since the native path sends no password in
//      the DPB (see ArchitectureComparison.cs);
//   4. the Services API, which the managed client also speaks itself;
//   5. the error model: a status vector becomes one FbException with the
//      entries in Errors, the first GDS code in ErrorCode, and SQLSTATE.
//
// Run:  cd samples/csharp && dotnet run -- ApiStyles
//
using System.Data.Common;
using System.Reflection;
using System.Runtime.InteropServices;
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Services;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class ApiStyles
{
    const string Sql = "select rdb$get_context('SYSTEM', 'ENGINE_VERSION') from rdb$database";

    [DllImport("libc", EntryPoint = "setenv")]
    static extern int SetEnv(string name, string value, int overwrite);

    /// <summary>The provider's internal per-protocol database class (reflection; 10.3.4 internals).</summary>
    static string InnerDatabase(FbConnection con)
    {
        const BindingFlags Any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        var inner = typeof(FbConnection).GetField("_innerConnection", Any)!.GetValue(con)!;
        var db = inner.GetType().GetProperty("Database", Any)!.GetValue(inner)!;
        return db.GetType().FullName!.Replace("FirebirdSql.Data.Client.", "");
    }

    public static void Run(string[] args)
    {
        var employee = Builder("employee", "NONE");

        // -- 1. portable ADO.NET ------------------------------------------------
        DbProviderFactory factory = FirebirdClientFactory.Instance;
        using (DbConnection con = factory.CreateConnection()!)
        {
            con.ConnectionString = employee.ToString();
            con.Open();
            using DbCommand cmd = con.CreateCommand();
            cmd.CommandText = Sql;
            Console.WriteLine($"[ADO.NET DbProviderFactory] engine version = {cmd.ExecuteScalar()}");
        }

        // -- 2. the provider's own types, same managed client -------------------
        using (var con = new FbConnection(employee.ToString()))
        {
            con.Open();
            using var cmd = new FbCommand(Sql, con);
            using var r = cmd.ExecuteReader();
            r.Read();
            var info = new FbDatabaseInfo(con);
            Console.WriteLine($"[FbCommand / FbDataReader ] engine version = {r.GetString(0)}   " +
                              $"({r.GetDataTypeName(0)}, protocol {info.GetProtocolVersion()}, {InnerDatabase(con)})");
        }

        // -- 3. the same ADO.NET calls over libfbclient's ISC API ---------------
        SetEnv("ISC_USER", User, 1);
        SetEnv("ISC_PASSWORD", Password, 1);
        var native = Builder("[::1]:employee", "NONE");
        native.ServerType = FbServerType.Embedded;
        native.ClientLibrary = "/opt/firebird/lib/libfbclient.so";
        using (var con = new FbConnection(native.ToString()))
        {
            con.Open();
            Console.WriteLine($"[ServerType=Embedded      ] engine version = {Scalar(con, Sql)}   " +
                              $"({InnerDatabase(con)}: isc_* calls via P/Invoke)");
        }

        // -- 4. the Services API ------------------------------------------------
        var props = new FbServerProperties(ServiceConnectionString());
        Console.WriteLine($"[FbServerProperties       ] server version = {props.GetServerVersion()}");

        // -- 5. the error model -------------------------------------------------
        try
        {
            using var con = new FbConnection(Builder("/nonexistent/x.fdb").ToString());
            con.Open();
        }
        catch (FbException e)
        {
            Console.WriteLine("[error model              ] attach /nonexistent/x.fdb -> FbException");
            Console.WriteLine("    " + ErrorText(e).Replace("\n", "\n    "));
            Console.WriteLine($"    ErrorCode {e.ErrorCode}  SQLSTATE {e.SQLSTATE}  " +
                              $"Errors[{e.Errors.Count}].Number = {string.Join(" ", e.Errors.Cast<FbError>().Select(x => x.Number))}");
        }
        Console.WriteLine("one provider: its own wire protocol AND libfbclient's ISC API. done.");
    }
}
