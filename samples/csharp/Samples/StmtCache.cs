//
// StmtCache.cs - the DSQL statement cache inferred from prepare timings
// (C# twin of ../../cpp/stmt_cache.cpp; see ../../../statement-cache.md).
//
// The same four timing runs on a statement that is heavy to COMPILE (a
// six-way self-join) and never executed: identical text (hits), the same
// text plus i trailing spaces (misses: the key is the text verbatim), a
// distinct literal (misses), and identical text after an unrelated
// RECREATE TABLE commit (misses: any DDL commit purges the cache).
// FbCommand.Prepare() is a real prepare-without-execute (op_allocate +
// op_prepare with the describe items) and Dispose() frees the handle;
// FirebirdClient keeps no client-side statement cache - ADO.NET's pooling
// is of connections, not statements - so nothing stands between the
// timings and the server's cache.  As in the Java and Go twins the
// prepares run in one explicit transaction, so run 4's DDL comes from a
// second attachment (the cache is per-database, so another attachment's
// DDL commit purges this one's entries too), and the .NET JIT is warmed on
// a cheap text first so run 1 times the server, not the provider's own
// first-call compilation.
//
// Run:  cd samples/csharp && dotnet run -- StmtCache [database]
//
using System.Diagnostics;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class StmtCache
{
    const int N = 100;
    const string Heavy = "SELECT COUNT(*) FROM t a"
                         + " JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id"
                         + " JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id"
                         + " JOIN t f ON e.id = f.id WHERE a.id > 0";

    static void PrepareOnce(FbConnection con, FbTransaction tx, string sql)
    {
        using var cmd = new FbCommand(sql, con, tx);
        cmd.Prepare();                                   // prepare only; Dispose frees the handle
    }

    /// <summary>Milliseconds for N prepares of the generated texts, optionally with DDL in between.</summary>
    static double Time(FbConnection con, FbTransaction tx, FbConnection? ddl, Func<int, string> text)
    {
        var total = TimeSpan.Zero;
        for (var i = 0; i < N; i++)
        {
            if (ddl != null)
                Execute(ddl, "RECREATE TABLE unrelated (x INT)");   // auto-commit: purges the cache
            var sql = text(i);
            var sw = Stopwatch.StartNew();
            PrepareOnce(con, tx, sql);
            total += sw.Elapsed;
        }
        return total.TotalMilliseconds;
    }

    static void Report(string label, double ms, string verdict) =>
        Console.WriteLine($"{label,-29} {N,3} prepares: {ms,6:F1} ms  ({ms / N:F2} ms/prepare) - {verdict}");

    public static void Run(string[] args)
    {
        var path = DbPath("stmt_cache", args);
        using var con = AttachOrCreate(path);
        using var ddl = Attach(path);
        Execute(con, "RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)");
        Execute(con, "EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO"
                     + " BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END");

        using var tx = con.BeginTransaction();
        // Warm the JIT on a cheap text, then the server's cache with the exact heavy text.
        for (var i = 0; i < 2000; i++)
            PrepareOnce(con, tx, "SELECT 1 FROM rdb$database");
        PrepareOnce(con, tx, Heavy);

        Report("1. identical text", Time(con, tx, null, _ => Heavy), "hits");
        Report("2. + i trailing spaces", Time(con, tx, null, i => Heavy + new string(' ', i + 1)), "misses");
        Report("3. distinct literal", Time(con, tx, null, i => Heavy.Replace("> 0", $"> {i}")), "misses");
        Report("4. identical text after DDL", Time(con, tx, ddl, _ => Heavy), "misses");
        tx.Commit();
    }
}
