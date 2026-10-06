//
// Types.cs - the headline Firebird types through ADO.NET (C# twin of
// ../../cpp/types.cpp; see ../../../sql-dialect-and-types.md).
//
// The same showcase table: BOOLEAN, INT128 at its maximum, DECFLOAT(34)
// holding an exact 0.1, TIMESTAMP WITH TIME ZONE with a named zone, and a
// CHECK-constrained domain.  Three faces of each column: the provider's
// own type (GetSchemaTable()'s ProviderType, an FbDbType whose members
// mirror the wire's SQL_* codes - Int128, Dec34, TimeStampTZ; the numeric
// code itself stays inside the provider), the Firebird type name
// (GetDataTypeName) and the CLR type (GetFieldType), then the fetched
// value.  .NET's BCL has no 34-digit decimal float and no zoned timestamp,
// so the provider brings its own structs: DECFLOAT arrives as an
// FbDecFloat - Coefficient (BigInteger) and Exponent, here exactly 1 x
// 10^-1 - and TIMESTAMP WITH TIME ZONE as an FbZonedDateTime (UTC DateTime
// + zone name, see Temporal.cs); INT128 is a System.Numerics.BigInteger.
// The domain violation is an FbException with SQLSTATE and gds code.
//
// Run:  cd samples/csharp && dotnet run -- Types [database]
//
using System.Data;
using System.Numerics;
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Types;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Types
{
    public static void Run(string[] args)
    {
        var path = DbPath("types", args);
        using var con = AttachOrCreate(path);

        // Each statement auto-commits, so each drop is committed before the next (DFW).
        foreach (var s in new[] { "DROP TABLE showcase", "DROP DOMAIN d_email" })
            try { Execute(con, s); } catch (FbException) { /* did not exist */ }
        Execute(con, "CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')");
        Execute(con, "CREATE TABLE showcase ("
                     + "  flag  BOOLEAN,"
                     + "  big   INT128,"
                     + "  money DECFLOAT(34),"
                     + "  born  TIMESTAMP WITH TIME ZONE,"
                     + "  mail  d_email)");
        Execute(con, "INSERT INTO showcase VALUES ("
                     + "  TRUE,"
                     + "  170141183460469231731687303715884105727,"
                     + "  0.1,"
                     + "  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
                     + "  'user@example.com')");

        try
        {
            Execute(con, "INSERT INTO showcase (mail) VALUES ('not-an-address')");
            Console.WriteLine("BUG: domain CHECK did not fire");
        }
        catch (FbException e)
        {
            Console.WriteLine("domain CHECK rejected 'not-an-address':");
            Console.WriteLine($"    SQLSTATE {e.SQLSTATE}, gds {e.ErrorCode}: {ErrorText(e).Split('\n')[0]}");
        }

        using var tx = con.BeginTransaction();
        using var cmd = new FbCommand("SELECT * FROM showcase", con, tx);
        using var r = cmd.ExecuteReader();
        var schema = r.GetSchemaTable();
        Console.WriteLine();
        Console.WriteLine($"{"column",-6} {"FbDbType (ProviderType)",-24} {"GetDataTypeName",-25} GetFieldType");
        Console.WriteLine($"{"------",-6} {"-----------------------",-24} {"-------------------------",-25} ------------");
        for (var i = 0; i < r.FieldCount; i++)
        {
            var provider = (FbDbType)(int)schema.Rows[i]["ProviderType"];
            Console.WriteLine($"{r.GetName(i),-6} {provider,-24} {r.GetDataTypeName(i),-25} {r.GetFieldType(i)}");
        }

        r.Read();
        var big = r.GetFieldValue<BigInteger>(r.GetOrdinal("BIG"));
        var money = (FbDecFloat)r["MONEY"];
        var born = (FbZonedDateTime)r["BORN"];
        Console.WriteLine();
        Console.WriteLine("typed round-trip:");
        Console.WriteLine($"  FLAG  {r["FLAG"]}  ({r["FLAG"].GetType().Name})");
        Console.WriteLine($"  BIG   {big}  == 2^127 - 1 ? {big == BigInteger.Pow(2, 127) - 1}");
        Console.WriteLine($"  MONEY {money}  (FbDecFloat: Coefficient={money.Coefficient}, Exponent={money.Exponent})"
                          + $"  == new FbDecFloat(1, -1) ? {money.Equals(new FbDecFloat(1, -1))}");
        Console.WriteLine($"        as double 0.1 the same value would be {0.1:G17}");
        Console.WriteLine($"  BORN  {born.DateTime:yyyy-MM-dd HH:mm:ss}Z {born.TimeZone}  ({born.GetType().Name})");
        Console.WriteLine($"  MAIL  \"{r["MAIL"]}\"");
        r.Close();
        tx.Commit();

        Console.WriteLine();
        Console.WriteLine("done.");
    }
}
