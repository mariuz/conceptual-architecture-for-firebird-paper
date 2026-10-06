//
// Temporal.cs - WITH TIME ZONE storage and the session zone (C# twin of
// ../../cpp/temporal.cpp; see ../../../temporal-and-time-zones.md).
//
// The same four steps: a named-zone and an offset TIMESTAMP WITH TIME ZONE
// literal (on the wire: UTC instant + 2-byte zone id), a DST conversion,
// instant equality, and SET TIME ZONE.  FirebirdClient decodes the wire
// struct itself and hides it: a fetch yields an FbZonedDateTime, a UTC
// DateTime (Kind=Utc) plus the zone NAME, resolved from the id through the
// provider's compiled-in copy of Firebird's zone table.  The numbers the C++
// sample memcpy's out of its buffer are therefore recomputed here from that
// DateTime, and the id comes from the server's own table, RDB$TIME_ZONES.
// The instructive difference is the offset literal: the provider's table
// holds only the named regions, so an offset-encoded id (1439 + minutes)
// fails the fetch with "Unknown time zone ID." - the sample reads that
// column on its own attachment (a fetch that dies mid-row leaves the wire
// out of step) and then falls back to a VARCHAR cast.  SET BIND OF TIME
// ZONE TO EXTENDED adds the offset to the value (FbZonedDateTime.Offset)
// but does not cure it.  The provider sends no isc_dpb_session_time_zone,
// so the session zone is the server's default until SET TIME ZONE.
//
// Run:  cd samples/csharp && dotnet run -- Temporal [database]
//
using FirebirdSql.Data.FirebirdClient;
using FirebirdSql.Data.Types;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Temporal
{
    static readonly DateTime IscEpoch = new(1858, 11, 17, 0, 0, 0, DateTimeKind.Utc);

    /// <summary>One value fetched as the provider's native type, in its own transaction.</summary>
    static object FetchTyped(FbConnection con, string expr)
    {
        using var tx = con.BeginTransaction();
        using var cmd = new FbCommand($"select {expr} from rdb$database", con, tx);
        using var r = cmd.ExecuteReader();
        r.Read();
        var v = r.GetValue(0);
        tx.Commit();
        return v;
    }

    static void Show(FbConnection con, string label, string zone, FbZonedDateTime z)
    {
        var utc = z.DateTime;
        var days = (utc.Date - IscEpoch).Days;
        var time = utc.TimeOfDay.Ticks / 1000;          // 100 ns ticks -> ISC 1/10000 s
        var id = Scalar(con, $"select rdb$time_zone_id from rdb$time_zones where rdb$time_zone_name = '{zone}'");
        Console.WriteLine($"{label} literal:");
        Console.WriteLine($"  as fetched  : {z.GetType().Name} DateTime={utc:yyyy-MM-dd HH:mm:ss} (Kind={utc.Kind}) TimeZone={z.TimeZone}");
        Console.WriteLine($"  as ISC      : UTC days={days} time={time}  zone id={Text(id)}  (recomputed; id from RDB$TIME_ZONES)");
        var local = TimeZoneInfo.ConvertTimeFromUtc(utc, TimeZoneInfo.FindSystemTimeZoneById(z.TimeZone));
        Console.WriteLine($"  wall clock  : {local:yyyy-MM-dd HH:mm:ss} {z.TimeZone}  (via .NET's tz database)");
    }

    static string Session(FbConnection con) =>
        $"session zone: {Text(Scalar(con, "select rdb$get_context('SYSTEM', 'SESSION_TIMEZONE') from rdb$database")),-18}"
        + $" CURRENT_TIMESTAMP: {Scalar(con, "select cast(current_timestamp as varchar(50)) from rdb$database")}";

    public static void Run(string[] args)
    {
        var path = DbPath("temporal", args);
        using var con = AttachOrCreate(path);

        // -- 1. Named zone: instant + region name survive.
        var named = (FbZonedDateTime)FetchTyped(con, "timestamp '2026-07-18 12:00:00 America/New_York'");
        Show(con, "named-zone", "America/New_York", named);

        // -- 2. Offset zone: an id the provider's table does not know.
        Console.WriteLine("offset literal:");
        var offsetLit = "timestamp '2026-07-18 12:00:00 -05:00'";
        var probe = Attach(path);                       // sacrificial: a failed fetch desyncs the wire
        try
        {
            var v = FetchTyped(probe, offsetLit);
            Console.WriteLine($"  as fetched  : {v}");
        }
        catch (ArgumentException e)
        {
            Console.WriteLine($"  as fetched  : {e.GetType().Name}: {e.Message}");
        }
        finally
        {
            try { probe.Dispose(); } catch (Exception) { /* the broken attachment's goodbye */ }
        }
        Console.WriteLine($"  as VARCHAR  : {Scalar(con, $"select cast({offsetLit} as varchar(40)) from rdb$database")}"
                          + $"  (offset id = 1439 + {(-5 * 60)} = {1439 - 5 * 60})");

        // -- 3. DST: the same NY wall time, two UTC instants.
        Console.WriteLine();
        foreach (var (season, date) in new[] { ("winter", "2026-01-18"), ("summer", "2026-07-18") })
        {
            var utc = (FbZonedDateTime)FetchTyped(con,
                $"timestamp '{date} 12:00:00 America/New_York' at time zone 'Etc/UTC'");
            Console.WriteLine($"NY 12:00 in UTC, {season}: {utc.DateTime:yyyy-MM-dd HH:mm:ss} {utc.TimeZone}");
        }
        Console.WriteLine("10:00 -02:00 = 09:00 -03:00 ? " + Text(Scalar(con,
            "select iif(time '10:00:00 -02:00' = time '09:00:00 -03:00', 'EQUAL', 'different') from rdb$database")).Trim());

        // -- 4. The session zone: no DPB item from this provider, so the server's default.
        Console.WriteLine();
        Console.WriteLine($"client (.NET) zone: {TimeZoneInfo.Local.Id}");
        Console.WriteLine(Session(con) + "   <- provider default (server's zone)");
        Execute(con, "set time zone 'Asia/Tokyo'");
        Console.WriteLine(Session(con) + "   <- SET TIME ZONE");

        Console.WriteLine();
        Console.WriteLine("done.");
    }
}
