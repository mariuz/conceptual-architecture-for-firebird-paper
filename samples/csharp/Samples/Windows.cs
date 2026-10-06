//
// Windows.cs - window functions and modern aggregates (C# twin of
// ../../cpp/windows.cpp; see ../../../aggregate-and-window-functions.md).
//
// Recreates the document's six-row sales table and runs its flagship
// analytics: the ranking / framed running total / LAG window query,
// FILTER + LISTAGG + STDDEV_POP, PERCENTILE_CONT and a hypothetical-set
// RANK(175) WITHIN GROUP, and Firebird 6's EXCLUDE CURRENT ROW frame.
//
// Window functions are plain SQL, so the differences are in the provider.
// The six rows go in through FbBatchCommand, which is Firebird 4's batch
// interface (IBatch / op_batch_*) on the wire: one prepared INSERT, six
// parameter sets, one round trip, a per-row result.  Every NUMERIC arrives
// as a System.Decimal (the INT128-wide running SUM included, no CAST),
// PERCENTILE_CONT is a DOUBLE.  And like Jaybird the provider has a plan
// API: FbCommand.GetCommandPlan() is the legacy plan the C++ sample prints
// with IStatement::getPlan(false), GetCommandExplainedPlan() the
// structured one.
//
// Run:  cd samples/csharp && dotnet run -- Windows [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Windows
{
    const string WindowSql =
        "SELECT region, amount," +
        " ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount) AS rn," +
        " RANK() OVER (ORDER BY amount DESC) AS overall_rank," +
        " SUM(amount) OVER (PARTITION BY region ORDER BY id" +
        "   ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_total," +
        " LAG(amount) OVER (PARTITION BY region ORDER BY id) AS prev_amount" +
        " FROM sales";

    /// <summary>Print a query as an aligned table headed by its column names.</summary>
    static void Print(FbConnection con, FbTransaction tx, string sql)
    {
        using var cmd = new FbCommand(sql, con, tx);
        using var r = cmd.ExecuteReader();
        var table = new List<string[]> { Enumerable.Range(0, r.FieldCount).Select(r.GetName).ToArray() };
        while (r.Read())
            table.Add(Enumerable.Range(0, r.FieldCount).Select(i => Text(r.IsDBNull(i) ? null : r.GetValue(i))).ToArray());
        var widths = Enumerable.Range(0, r.FieldCount).Select(i => table.Max(row => row[i].Length)).ToArray();
        string Line(string[] cells) => string.Join(" ", cells.Select((c, i) => c.PadRight(widths[i]))).TrimEnd();
        Console.WriteLine(Line(table[0]));
        Console.WriteLine(Line(widths.Select(w => new string('-', w)).ToArray()));
        foreach (var row in table.Skip(1))
            Console.WriteLine(Line(row));
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("windows", args));
        Execute(con, "RECREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(10), amount NUMERIC(10,2))");

        using var tx = con.BeginTransaction();
        using (var batch = new FbBatchCommand("INSERT INTO sales VALUES (@id, @region, @amount)", con, tx))
        {
            (int, string, decimal)[] rows =
            {
                (1, "East", 100), (2, "East", 200), (3, "East", 150),
                (4, "West", 300), (5, "West", 250), (6, "West", 400),
            };
            foreach (var (id, region, amount) in rows)
            {
                var p = batch.AddBatchParameters();
                p.Add("@id", id);
                p.Add("@region", region);
                p.Add("@amount", amount);
            }
            var result = batch.ExecuteNonQuery();
            result.EnsureSuccess();
            Console.WriteLine($"batch: {result.Count} rows in one FbBatchCommand, all succeeded = {result.AllSuccess}");
        }

        // -- 1. The flagship window query: partitioned ranking, a framed
        //       running total, and LAG navigation - every row kept.
        Console.WriteLine("\n== window functions ==");
        Print(con, tx, WindowSql);
        using (var cmd = new FbCommand(WindowSql, con, tx))
        {
            cmd.Prepare();
            Console.WriteLine($"\nplan:{cmd.GetCommandPlan().TrimEnd()}");
            Console.WriteLine($"\nexplained plan:{cmd.GetCommandExplainedPlan().TrimEnd()}");
        }
        using (var cmd = new FbCommand("SELECT FIRST 1 running_total FROM (" + WindowSql + ")", con, tx))
            Console.WriteLine($"\nrunning_total arrives as {cmd.ExecuteScalar().GetType()}");

        // -- 2. Aggregates: FILTER (FB5), ordered LISTAGG, statistical.
        Console.WriteLine("\n== aggregates: FILTER / LISTAGG / STDDEV_POP ==");
        Print(con, tx,
            "SELECT region, COUNT(*) AS n," +
            " COUNT(*) FILTER (WHERE amount > 150) AS big_sales," +
            " CAST(LISTAGG(amount, ',') WITHIN GROUP (ORDER BY amount) AS VARCHAR(60)) AS amounts," +
            " CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev" +
            " FROM sales GROUP BY region");

        // -- 3. Ordered-set and hypothetical-set aggregates.
        Console.WriteLine("\n== PERCENTILE_CONT median / hypothetical RANK(175) ==");
        Print(con, tx,
            "SELECT region," +
            " PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median," +
            " RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175" +
            " FROM sales GROUP BY region");

        // -- 4. FB6 frame exclusion: the neighbours' average.
        Console.WriteLine("\n== FB6 frame EXCLUDE CURRENT ROW (neighbours' average) ==");
        Print(con, tx,
            "SELECT id, amount," +
            " CAST(AVG(amount) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING" +
            "   EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg" +
            " FROM sales");

        tx.Commit();
        Console.WriteLine("\ndone.");
    }
}
