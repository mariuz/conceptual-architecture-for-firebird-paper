//
// Ha.cs - the client-side HA primitive: a database SHADOW (C# twin of
// ../../cpp/ha.cpp; see ../../../high-availability.md).
//
// As in the C++ sample, a shadow is created on a fresh scratch database,
// shown in RDB$FILES, measured (FileInfo.Length - server and sample share a
// host here) while 5000 rows go in, and retired with DROP SHADOW ...
// DELETE FILE.  CREATE/DROP SHADOW are ordinary DSQL, so the provider's
// managed wire protocol needs nothing special.  Where the Java twin went
// one step further - promoting a replica by attaching with the
// isc_dpb_set_db_replica DPB item - this provider stops short: its
// connection string has no key for that DPB item (and there is no SQL
// form), so the sample can only *read* the mode, through the
// isc_info_replica_mode item that FbDatabaseInfo.GetReplicaMode() asks for.
// The shadow's recovery verbs are here, though, in the Services classes:
// FbConfiguration.ActivateShadows() is gfix -activate and
// FbValidationFlags.KillShadows is gfix -kill (named only: showing them
// needs a lost main file).
//
// Run:  cd samples/csharp && dotnet run -- Ha [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Ha
{
    static long Size(string path) => File.Exists(path) ? new FileInfo(path).Length : -1;

    static void ShowFiles(string when, string main, string shadow) =>
        Console.WriteLine($"{when,-28} main = {Size(main),8} bytes, shadow = {Size(shadow),8} bytes");

    /// <summary>Drop-and-create; this provider version throws NullReferenceException
    /// (not FbException) when DropDatabase finds no file, so catch both.</summary>
    static FbConnection Fresh(string path)
    {
        var cs = Builder(path).ToString();
        try { FbConnection.DropDatabase(cs); }
        catch (Exception e) when (e is FbException or NullReferenceException) { /* did not exist */ }
        FbConnection.CreateDatabase(cs, overwrite: true);
        return Attach(path);
    }

    public static void Run(string[] args)
    {
        var main = DbPath("ha", args);
        var shadow = Path.ChangeExtension(main, ".shd");
        using var con = Fresh(main);
        Execute(con, "CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))");

        // 1. Create the synchronous page-level mirror.
        Execute(con, $"CREATE SHADOW 1 '{shadow}'");
        Console.WriteLine("CREATE SHADOW 1 done - the engine dumped every page to the mirror");
        foreach (var row in Rows(con, "SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS "
                                    + "FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER"))
            Console.WriteLine($"RDB$FILES: {Text(row[0]).Trim()}  shadow_number={row[1]}  flags={row[2]}");
        ShowFiles("after CREATE SHADOW:", main, shadow);

        // 2. Write load: every page write now goes to both files.
        Execute(con, "EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
                   + "  WHILE (I < 5000) DO BEGIN "
                   + "    INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1; "
                   + "  END "
                   + "END");
        ShowFiles("after 5000 inserts:", main, shadow);

        // 3. Retire the mirror.
        Execute(con, "DROP SHADOW 1 DELETE FILE");
        Console.WriteLine("DROP SHADOW 1 DELETE FILE done");
        ShowFiles("after DROP SHADOW:", main, shadow);
        Console.WriteLine($"RDB$FILES rows left: {Scalar(con, "SELECT COUNT(*) FROM RDB$FILES")}");

        // 4. Replica mode: readable through the info API, not settable from here.
        var info = new FbDatabaseInfo(con);
        Console.WriteLine($"\nFbDatabaseInfo.GetReplicaMode(): {info.GetReplicaMode()}"
                          + $"  (MON$REPLICA_MODE = {Scalar(con, "SELECT MON$REPLICA_MODE FROM MON$DATABASE")})");
        Console.WriteLine("done.");
    }
}
