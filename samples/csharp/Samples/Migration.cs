//
// Migration.cs - the type-mapping table made concrete (C# twin of
// ../../cpp/migration.cpp; see ../../../migration-and-interoperability.md).
//
// A probe table with the types migrations trip over (INT128, NUMERIC(38,8),
// DECFLOAT(34), TIMESTAMP WITH TIME ZONE, BOOLEAN, CHAR(16) OCTETS as a
// UUID) is inspected on three faces: the DESCRIBED metadata, the native
// values, and the server-side CAST ... AS VARCHAR text face.  An ADO.NET
// tool sees the describe face through GetSchemaTable - the provider's
// FbDbType, the CLR DataType, size, precision and scale - which is what
// generic .NET copiers (DataTable fills, bulk loaders) map target types
// from.  The native face is where the managed provider bites twice, and
// both failures happen *inside the fetch*, leaving that attachment's wire
// stream out of step - so the sample reads each column on an attachment of
// its own:
//   - NUMERIC(38,8) is mapped to System.Decimal, whose 96-bit mantissa
//     holds 28-29 digits: the 38-digit value throws OverflowException.
//     The fix is server-side: SET BIND OF NUMERIC(38) TO VARCHAR (the
//     Firebird 4+ session coercion) delivers it as exact text.
//   - CHAR(16) OCTETS is described as System.Guid, but on a UTF8 connection
//     the provider declares the column in the connection charset when it
//     fetches, and the server answers "Malformed string" (on a NONE
//     connection it arrives as a Guid).  CAST(... AS BLOB SUB_TYPE BINARY)
//     sidesteps it.
// INT128 (BigInteger), DECFLOAT(34) (FbDecFloat: coefficient + exponent),
// TIMESTAMP WITH TIME ZONE (FbZonedDateTime: UTC DateTime + zone name) and
// BOOLEAN are decoded exactly.
//
// Run:  cd samples/csharp && dotnet run -- Migration [database]
//
using System.Data;
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Types;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Migration
{
    static string Show(object v) => v switch
    {
        FbDecFloat d => $"{d}  (coefficient {d.Coefficient}, exponent {d.Exponent})",
        FbZonedDateTime z => $"{z.DateTime:yyyy-MM-dd HH:mm:ss} UTC, zone {z.TimeZone}",
        byte[] b => $"{new Guid(b, bigEndian: true)}  ({b.Length} bytes -> new Guid(b, bigEndian: true))",
        _ => Text(v),
    };

    /// <summary>One column on an attachment of its own: a failed fetch poisons only that one.</summary>
    static void Native(string path, string label, string expr, string? setup = null, string charset = "UTF8")
    {
        using var con = Attach(path, charset);
        var note = setup ?? (expr != label ? expr : charset != "UTF8" ? $"Charset={charset}" : null);
        try
        {
            if (setup != null)
                Execute(con, setup);
            using var cmd = new FbCommand($"SELECT {expr} FROM TYPE_PROBE", con);
            using var r = cmd.ExecuteReader();
            r.Read();
            var v = r.GetValue(0);
            Console.WriteLine($"  {label,-8} -> {v.GetType().Name,-15} {Show(v)}" + (note != null ? $"   [{note}]" : ""));
        }
        catch (Exception e) when (e is OverflowException or FbException)
        {
            var msg = e is FbException fe ? ErrorText(fe).Replace("\n", "; ") : e.Message;
            Console.WriteLine($"  {label,-8} -> {e.GetType().Name}: {msg}" + (note != null ? $"   [{note}]" : ""));
        }
    }

    public static void Run(string[] args)
    {
        var path = DbPath("migration", args);
        using var con = AttachOrCreate(path);
        Execute(con, "RECREATE TABLE TYPE_PROBE ("
                   + "  C_INT128 INT128,"
                   + "  C_NUM    NUMERIC(38,8),"
                   + "  C_DEC    DECFLOAT(34),"
                   + "  C_TSTZ   TIMESTAMP WITH TIME ZONE,"
                   + "  C_BOOL   BOOLEAN,"
                   + "  C_UUID   CHAR(16) CHARACTER SET OCTETS,"
                   + "  C_VC     VARCHAR(20))");
        Execute(con, "INSERT INTO TYPE_PROBE VALUES ("
                   + "  170141183460469231731687303715884105727,"
                   + "  123456789012345678901234567890.12345678,"
                   + "  1.234567890123456789012345678901234E+10,"
                   + "  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
                   + "  TRUE, GEN_UUID(), 'naïve ütf8 text')");

        // -- 1. the describe face, as ADO.NET publishes it (no fetch) -------------
        using (var cmd = new FbCommand("SELECT * FROM TYPE_PROBE", con))
        using (var r = cmd.ExecuteReader(CommandBehavior.SchemaOnly))
        {
            Console.WriteLine("GetSchemaTable of SELECT * FROM TYPE_PROBE:\n");
            Console.WriteLine($"{"column",-8} {"ProviderType",-14} {"GetDataTypeName",-24} {"size",4} {"prec",4} {"scale",5}  DataType");
            foreach (DataRow c in r.GetSchemaTable().Rows)
            {
                var ordinal = (int)c["ColumnOrdinal"];
                Console.WriteLine($"{c["ColumnName"],-8} {(FbDbType)c["ProviderType"],-14} {r.GetDataTypeName(ordinal),-24} "
                                  + $"{c["ColumnSize"],4} {Text(c["NumericPrecision"]),4} {Text(c["NumericScale"]),5}  "
                                  + $"{((Type)c["DataType"]).FullName}");
            }
        }

        // -- 2. the native face: GetValue, one attachment per column ---------
        Console.WriteLine("\nsame row fetched natively (GetValue, a fresh attachment per column):\n");
        foreach (var c in new[] { "C_INT128", "C_NUM", "C_DEC", "C_TSTZ", "C_BOOL", "C_UUID", "C_VC" })
            Native(path, c, c);

        Console.WriteLine("\nthe two failures, worked around:\n");
        Native(path, "C_NUM", "C_NUM", "SET BIND OF NUMERIC(38) TO VARCHAR");
        Native(path, "C_UUID", "CAST(C_UUID AS BLOB SUB_TYPE BINARY)");
        Native(path, "C_UUID", "C_UUID", charset: "NONE");

        // -- 3. the text face: engine-rendered strings --------------------------
        Console.WriteLine("\nthe text face (server-side CAST ... AS VARCHAR):\n");
        foreach (var c in new[] { "C_INT128", "C_NUM", "C_DEC", "C_TSTZ", "C_BOOL", "C_VC" })
            Console.WriteLine($"  {c,-8} = {Scalar(con, $"SELECT CAST({c} AS VARCHAR(60)) FROM TYPE_PROBE")}");
        Console.WriteLine($"  {"C_UUID",-8} = {Scalar(con, "SELECT UUID_TO_CHAR(C_UUID) FROM TYPE_PROBE")}"
                          + "   (rendered via UUID_TO_CHAR)");
        Console.WriteLine("\ndone.");
    }
}
