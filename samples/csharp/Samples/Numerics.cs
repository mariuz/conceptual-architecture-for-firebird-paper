//
// Numerics.cs - exact and floating numerics on the wire (C# twin of
// ../../cpp/numerics.cpp; see ../../../numeric-and-precision-arithmetic.md).
//
// Four experiments: the residue of (0.1 + 0.2) - 0.3 in DOUBLE PRECISION vs
// DECFLOAT(34); NUMERIC(18,4) as a scaled integer plus a scale; INT128 at
// 2^127-1 and one step past it; the DECFLOAT Division_by_zero trap, on by
// default and then cleared.
//
// .NET's System.Decimal is itself a scaled integer (96-bit coefficient plus
// a power-of-ten scale), so FirebirdClient hands NUMERIC over without ever
// going through binary floating point, and decimal.GetBits shows the same
// 123456789 / scale 4 the C++ twin reads out of the message buffer.  INT128
// arrives as System.Numerics.BigInteger, DECFLOAT as the provider's own
// FbDecFloat (BigInteger coefficient + exponent) - which, unlike Jaybird's
// BigDecimal, has an Infinity.  The provider has no DPB knob for the trap
// set, so clearing traps is SQL only.
//
// Run:  cd samples/csharp && dotnet run -- Numerics [database]
//
using System.Data;
using System.Numerics;
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Types;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Numerics
{
    const string OneByZero = "select cast(1 as decfloat(16)) / 0 from rdb$database";

    static string Brief(FbException e) => ErrorText(e).Replace("\n", "; ");

    static string Fetch(FbConnection con, string sql)
    {
        try
        {
            var v = Scalar(con, sql);
            return $"{Text(v)}  ({v?.GetType().Name})";
        }
        catch (FbException e)
        {
            return $"{Brief(e)} (gds {e.ErrorCode})";
        }
        catch (Exception e)
        {
            return $"client-side {e.GetType().Name}: {e.Message}";
        }
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("numerics", args));

        // -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3 ------------------------
        var dbl = Scalar(con, "select (cast(0.1 as double precision) + 0.2) - 0.3 from rdb$database")!;
        var dec = (FbDecFloat)Scalar(con, "select (cast(0.1 as decfloat(34)) + 0.2) - 0.3 from rdb$database")!;
        Console.WriteLine($"(0.1+0.2)-0.3 in DOUBLE PRECISION : {(double)dbl:R}  ({dbl.GetType().Name})");
        Console.WriteLine($"(0.1+0.2)-0.3 in DECFLOAT(34)     : {dec}  (FbDecFloat: coefficient {dec.Coefficient}, exponent {dec.Exponent})");
        Console.WriteLine();

        // -- 2. NUMERIC(18,4): a scaled integer --------------------------------------
        using (var cmd = new FbCommand("select cast(12345.6789 as numeric(18,4)) from rdb$database", con))
        using (var r = cmd.ExecuteReader())
        {
            r.Read();
            var m = r.GetDecimal(0);
            var col = r.GetSchemaTable().Rows[0];
            Console.WriteLine($"NUMERIC(18,4) metadata   : ProviderType={(FbDbType)(int)col["ProviderType"]}, "
                              + $"ColumnSize={col["ColumnSize"]}, NumericScale={col["NumericScale"]}, DataType={col["DataType"]}");
            var bits = decimal.GetBits(m);
            var scale = (bits[3] >> 16) & 0xff;
            Console.WriteLine($"System.Decimal bits      : lo=0x{bits[0]:x8} mid={bits[1]} hi={bits[2]} scale={scale}");
            Console.WriteLine($"raw integer              : {bits[0]}");
            Console.WriteLine($"value = raw * 10^-scale  : {bits[0]} * 10^-{scale} = {m}");
        }
        var cent = (decimal)Scalar(con, "select cast(90071992547409.93 as numeric(18,2)) from rdb$database")!;
        Console.WriteLine($"NUMERIC(18,2) past 2^53  : {cent} as decimal, {(double)cent:R} as (double)decimal, "
                          + $"{double.Parse(cent.ToString(System.Globalization.CultureInfo.InvariantCulture), System.Globalization.CultureInfo.InvariantCulture):R} parsed");
        Console.WriteLine();

        // -- 3. INT128: the full range, and one step past it -------------------------
        var max = Scalar(con, "select cast(170141183460469231731687303715884105727 as int128) from rdb$database")!;
        Console.WriteLine($"INT128 max  : {max}  ({max.GetType().Name}, == 2^127-1: {(BigInteger)max == BigInteger.Pow(2, 127) - 1})");
        try
        {
            Scalar(con, "select cast(170141183460469231731687303715884105727 as int128) + 1 from rdb$database");
            Console.WriteLine("BUG: overflow not detected");
        }
        catch (FbException e)
        {
            Console.WriteLine($"INT128 max+1: {Brief(e)} (gds {e.ErrorCode})");
        }
        Console.WriteLine();

        // -- 4. DECFLOAT division by zero ------------------------------------------
        try
        {
            Scalar(con, OneByZero);
            Console.WriteLine("BUG: default trap did not fire");
        }
        catch (FbException e)
        {
            Console.WriteLine($"1/0 with default traps : {Brief(e)} (gds {e.ErrorCode})");
        }
        Execute(con, "set decfloat traps to");                  // clear all traps (session-level)
        var inf = (FbDecFloat)Scalar(con, OneByZero)!;
        Console.WriteLine($"1/0 with traps cleared : {inf}  (FbDecFloat, == PositiveInfinity: {inf == FbDecFloat.PositiveInfinity})");

        // -- 5. past System.Decimal: NUMERIC(38) wider than 96 bits ------------------
        // A separate attachment: the client-side overflow strikes mid-fetch and
        // leaves that attachment's implicit transaction unusable.
        using (var wide = Attach(DbPath("numerics", args)))
        {
            Console.WriteLine($"NUMERIC(38,0) = 10^30 : {Fetch(wide, "select cast(1e30 as numeric(38,0)) from rdb$database")}");
            Console.WriteLine($"  same, cast to INT128 : {Fetch(wide, "select cast(cast(1e30 as numeric(38,0)) as int128) from rdb$database")}");
        }
        Console.WriteLine("done.");
    }
}
