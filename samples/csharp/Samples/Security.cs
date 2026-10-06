//
// Security.cs - the security layers observed from client code (C# twin of
// ../../cpp/security.cpp; see ../../../security-architecture.md).
//
// The same four steps: the attachment's own MON$ATTACHMENTS row (layers 1+2
// as the server recorded them), SEC$USERS, a temporary user plus a role
// carrying MONITOR_ANY_ATTACHMENT (attached without and then with the role),
// and a failed login.  FirebirdClient's default managed wire protocol
// implements layers 1 and 2 itself - its own Srp256 client proof and its
// own wire encryption (FbConnectionStringBuilder.WireCrypt: Disabled /
// Enabled / Required) - so the cipher the server records is the
// provider's choice, not fbclient's.  The role is the builder's Role
// property (the DPB's isc_dpb_sql_role_name); the wrong password arrives as
// an FbException carrying SQLSTATE 28000 and gds 335544472 (isc_login).
// Each DDL statement runs without an explicit transaction, so the provider
// commits it at once - a full commit, which user management (deferred work
// executed AT commit) needs.  Names CS_USER / CS_MONITOR let it run beside
// the other twins.
//
// Run:  cd samples/csharp && dotnet run -- Security [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Security
{
    const string User_ = "CS_USER";
    const string Pass = "Hands0nPw";
    const string Role = "CS_MONITOR";

    static void WhoAmI(FbConnection con, string label)
    {
        var r = Rows(con, "select trim(mon$user), mon$auth_method, mon$wire_crypt_plugin,"
                          + " mon$remote_protocol, trim(coalesce(current_role, 'NONE'))"
                          + " from mon$attachments where mon$attachment_id = current_connection")[0];
        Console.WriteLine($"{label,-22} user={r[0]} auth={r[1]} wirecrypt={Text(r[2])} protocol={r[3]} role={r[4]}");
    }

    static object? Visible(FbConnection con) =>
        Scalar(con, "select count(*) from mon$attachments where mon$system_flag = 0");

    static FbConnection As(string path, string user, string pass, string? role = null)
    {
        var b = Builder(path);
        b.UserID = user;
        b.Password = pass;
        if (role != null)
            b.Role = role;                             // -> isc_dpb_sql_role_name
        var con = new FbConnection(b.ToString());
        con.Open();
        return con;
    }

    /// <summary>Cleanup of a previous run, only for what exists (DROP USER of a
    /// missing user would fail only at commit).</summary>
    static void Cleanup(FbConnection con)
    {
        if (Convert.ToInt32(Scalar(con, $"select count(*) from sec$users where sec$user_name = '{User_}'")) > 0)
            Execute(con, $"drop user {User_} using plugin Srp");
        if (Convert.ToInt32(Scalar(con, $"select count(*) from rdb$roles where rdb$role_name = '{Role}'")) > 0)
            Execute(con, $"drop role {Role}");
    }

    public static void Run(string[] args)
    {
        var path = DbPath("security", args);
        using var admin = AttachOrCreate(path);

        // 1. Layers 1+2, as recorded for THIS attachment.
        WhoAmI(admin, "admin attachment:");

        // 2+3. One statement, one full (auto) commit: the user exists only after it.
        Cleanup(admin);
        Execute(admin, $"create user {User_} password '{Pass}' using plugin Srp");
        Execute(admin, $"create role {Role} set system privileges to MONITOR_ANY_ATTACHMENT");
        Execute(admin, $"grant {Role} to user {User_}");

        Console.WriteLine();
        Console.WriteLine("SEC$USERS (the security database, through the virtual view):");
        Console.WriteLine($"    {"USER",-16} {"PLUGIN",-8} ADMIN");
        foreach (var r in Rows(admin, "select trim(sec$user_name), trim(sec$plugin), sec$admin from sec$users order by 1"))
            Console.WriteLine($"    {r[0],-16} {r[1],-8} {r[2]}");

        Console.WriteLine();
        Console.WriteLine($"admin sees {Visible(admin)} user attachments in MON$ATTACHMENTS");

        using (var plain = As(path, User_, Pass))
        {
            WhoAmI(plain, "user, no role:");
            Console.WriteLine($"  -> sees {Visible(plain)} attachment(s): only its own");
        }
        using (var monitor = As(path, User_, Pass, Role))
        {
            WhoAmI(monitor, "user + role:");
            Console.WriteLine($"  -> sees {Visible(monitor)} attachments: MONITOR_ANY_ATTACHMENT at work");
        }

        // 4. The failed login.
        Console.WriteLine();
        Console.WriteLine("failed login (wrong password) produces:");
        try
        {
            using var bad = As(path, User_, "wrong-password");
            Console.WriteLine("    unexpected: login succeeded");
        }
        catch (FbException e)
        {
            Console.WriteLine($"    SQLSTATE {e.SQLSTATE} / gds {e.ErrorCode} ({e.GetType().Name}, {e.Errors.Count} status entries)");
            Console.WriteLine("    " + ErrorText(e).Replace("\n", "\n    "));
        }

        Execute(admin, $"drop user {User_} using plugin Srp");
        Execute(admin, $"drop role {Role}");
        Console.WriteLine();
        Console.WriteLine("temporary user and role dropped. done.");
    }
}
