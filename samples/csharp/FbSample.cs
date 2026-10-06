//
// FbSample.cs - shared boilerplate for the C# hands-on twins.
//
// Every sample demonstrates one companion document of the paper.  They use
// FirebirdSql.Data.FirebirdClient (https://github.com/FirebirdSQL/NETProvider),
// the FirebirdSQL project's ADO.NET provider.  Its default server type is a
// managed implementation of the wire protocol (no libfbclient, like the Go,
// JavaScript and pure-Java drivers); ServerType=Embedded loads the native
// client library / engine in-process.  Beyond ADO.NET it carries
// Firebird's own layer: FbTransactionOptions (every TPB item), the Services
// classes in FirebirdSql.Data.Services (backup, restore, statistics,
// validation, configuration, trace, security, nbackup), FbRemoteEvent, and
// FbCommand.GetCommandPlan / GetCommandExplainedPlan.
//
// Like the other twins, everything runs against the local server with
// scratch databases under /tmp/fbhandson (SYSDBA/masterkey, overridable via
// ISC_USER / ISC_PASSWORD; FB_HOST picks another server).
//
using System.Reflection;
using FirebirdSql.Data.FirebirdClient;

namespace FbSamples;

public static class FbSample
{
    public const string Scratch = "/tmp/fbhandson";
    public static readonly string Host = Env("FB_HOST", "localhost");
    public static readonly string User = Env("ISC_USER", "SYSDBA");
    public static readonly string Password = Env("ISC_PASSWORD", "masterkey");

    static string Env(string name, string def) =>
        Environment.GetEnvironmentVariable(name) is { Length: > 0 } v ? v : def;

    /// <summary>Server path of a topic's scratch database; args[0] overrides it.</summary>
    public static string DbPath(string topic, string[] args) =>
        args.Length > 0 ? args[0] : $"{Scratch}/{topic}_cs.fdb";

    /// <summary>A connection string builder for a server path or alias.</summary>
    public static FbConnectionStringBuilder Builder(string database, string charset = "UTF8") => new()
    {
        DataSource = Host,
        Database = database,
        UserID = User,
        Password = Password,
        Charset = charset,
        Pooling = false,   // one FbConnection = one attachment, unless a sample says otherwise
    };

    /// <summary>Attach to an existing database.</summary>
    public static FbConnection Attach(string database, string charset = "UTF8")
    {
        var con = new FbConnection(Builder(database, charset).ToString());
        con.Open();
        return con;
    }

    /// <summary>Attach, creating the database first if it does not exist.</summary>
    public static FbConnection AttachOrCreate(string database, string charset = "UTF8")
    {
        try
        {
            return Attach(database, charset);
        }
        catch (FbException)
        {
            FbConnection.CreateDatabase(Builder(database, charset).ToString(), overwrite: false);
            return Attach(database, charset);
        }
    }

    /// <summary>A fresh scratch database: drop it if it exists, then create it.</summary>
    public static FbConnection Recreate(string database, string charset = "UTF8")
    {
        var cs = Builder(database, charset).ToString();
        // DropDatabase of a missing file throws NullReferenceException, not
        // FbException, in FirebirdClient 10.3.4 - so catch broadly.
        try { FbConnection.DropDatabase(cs); } catch (Exception) { /* did not exist */ }
        FbConnection.CreateDatabase(cs, overwrite: true);
        return Attach(database, charset);
    }

    /// <summary>The demo server's employee database (charset NONE, like its data).</summary>
    public static FbConnection Employee() => Attach(Env("FB_DATABASE", "employee"), "NONE");

    /// <summary>Services API connection string (service_mgr on Host).</summary>
    public static string ServiceConnectionString(string? database = null)
    {
        var b = Builder(database ?? "", "UTF8");
        return b.ToString();
    }

    /// <summary>Run a statement (in tx, or auto-committed when tx is null).</summary>
    public static int Execute(FbConnection con, string sql, FbTransaction? tx = null)
    {
        using var cmd = new FbCommand(sql, con, tx);
        return cmd.ExecuteNonQuery();
    }

    /// <summary>The first column of the first row.</summary>
    public static object? Scalar(FbConnection con, string sql, FbTransaction? tx = null)
    {
        using var cmd = new FbCommand(sql, con, tx);
        var v = cmd.ExecuteScalar();
        return v is DBNull ? null : v;
    }

    /// <summary>Every row of a query as object?[].</summary>
    public static List<object?[]> Rows(FbConnection con, string sql, FbTransaction? tx = null)
    {
        using var cmd = new FbCommand(sql, con, tx);
        using var r = cmd.ExecuteReader();
        var rows = new List<object?[]>();
        while (r.Read())
        {
            var row = new object?[r.FieldCount];
            for (var i = 0; i < r.FieldCount; i++)
                row[i] = r.IsDBNull(i) ? null : r.GetValue(i);
            rows.Add(row);
        }
        return rows;
    }

    /// <summary>A value rendered for display, NULL as &lt;null&gt;.</summary>
    public static string Text(object? v) => v switch
    {
        null or DBNull => "<null>",
        byte[] b => Convert.ToHexString(b),
        _ => Convert.ToString(v, System.Globalization.CultureInfo.InvariantCulture) ?? "",
    };

    /// <summary>An FbException's message, one status-vector entry per line.</summary>
    public static string ErrorText(FbException e) =>
        string.Join("\n", e.Message.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));

    /// <summary>Run a sample, exiting 1 with the engine's message on error.</summary>
    public static int Guard(Action body)
    {
        try
        {
            body();
            return 0;
        }
        catch (Exception e)
        {
            var inner = e is TargetInvocationException { InnerException: { } ie } ? ie : e;
            Console.Error.WriteLine(inner is FbException fe ? "error: " + ErrorText(fe) : inner.ToString());
            return 1;
        }
    }
}
