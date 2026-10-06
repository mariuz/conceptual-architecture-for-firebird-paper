//
// Profiler.cs - the profiler's accumulation view, driven from client code
// (C# twin of ../../cpp/profiler.cpp; see ../../../profiler.md).
//
// One RDB$PROFILER session brackets two workloads - a self-join over a
// 5,000-row table and a 20,000-iteration PSQL loop - and the PLG$PROFILER
// schema is then queried like any other data: the record-source view as an
// indented plan tree, the PSQL view ranked by time per line and column.
//
// ADO.NET loses nothing either: the control surface is a SQL package and
// the output a SQL schema.  The C#-specific details: the procedure is a raw
// string literal, whose closing """ marks the indentation to strip, so the
// relative indentation - and with it the column numbers the profiler
// reports - survives; the profile id is bound as a named @id parameter;
// and the session runs in an explicit SNAPSHOT FbTransaction, so the
// sample can show the autonomous-flush pitfall (a count in that same
// transaction after FINISH_SESSION(TRUE) sees nothing) before a real
// Commit() makes the flush visible.
//
// Run:  cd samples/csharp && dotnet run -- Profiler [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Profiler
{
    const string Hotspot = """
        create procedure hotspot returns (total bigint) as
          declare i int = 0;
          declare x int;
        begin
          total = 0;
          while (i < 20000) do
          begin
            select val from nums where id = mod(:i, 5000) into :x;
            total = total + coalesce(:x, 0);
            i = i + 1;
          end
          suspend;
        end
        """;

    static readonly FbTransactionOptions Snapshot = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency
                              | FbTransactionBehavior.Write
                              | FbTransactionBehavior.Wait,
    };

    /// <summary>Print a query with a bound profile id as an aligned table.</summary>
    static void Print(FbConnection con, FbTransaction tx, string sql, long profileId)
    {
        using var cmd = new FbCommand(sql, con, tx);
        cmd.Parameters.AddWithValue("@id", profileId);
        using var r = cmd.ExecuteReader();
        var table = new List<string[]> { Enumerable.Range(0, r.FieldCount).Select(r.GetName).ToArray() };
        while (r.Read())
            table.Add(Enumerable.Range(0, r.FieldCount).Select(i => Text(r.IsDBNull(i) ? null : r.GetValue(i))).ToArray());
        var w = Enumerable.Range(0, r.FieldCount).Select(i => table.Max(row => row[i].Length)).ToArray();
        for (var k = 0; k < table.Count; k++)
        {
            var row = table[k];
            Console.WriteLine(string.Join(" ", row.Select((c, i) => i == row.Length - 1 ? c : c.PadRight(w[i]))));
            if (k == 0)
                Console.WriteLine(string.Join(" ", w.Select(n => new string('-', n))));
        }
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("profiler", args));

        // --- workload fixtures (auto-commit: each DDL commits) ---------------------
        try { Execute(con, "drop procedure hotspot"); } catch (FbException) { /* first run */ }
        Execute(con, "recreate table nums (id int primary key, val int)");
        Execute(con, "execute block as declare n int = 0; begin "
                     + "  while (n < 5000) do begin "
                     + "    insert into nums values (:n, mod(:n, 97)); n = n + 1; end end");
        Execute(con, Hotspot);

        // --- profile inside one SNAPSHOT transaction -------------------------------
        long profileId;
        using (var tx = con.BeginTransaction(Snapshot))
        {
            profileId = Convert.ToInt64(Scalar(con, "select rdb$profiler.start_session('c# hands-on') from rdb$database", tx));
            Scalar(con, "select count(*) from nums a join nums b on b.id = a.val", tx);
            Scalar(con, "select total from hotspot", tx);
            Execute(con, "execute procedure rdb$profiler.finish_session(true)", tx);
            Console.WriteLine($"profile session {profileId} finished and flushed");
            var seen = Scalar(con, $"select count(*) from plg$profiler.plg$prof_psql_stats where profile_id = {profileId}", tx);
            Console.WriteLine($"PSQL stat rows visible inside the SNAPSHOT that ran it: {seen}"
                              + "  <- the flush committed autonomously, after the snapshot");
            tx.Commit();                                  // a real commit, not CommitRetaining
        }

        using (var tx = con.BeginTransaction(Snapshot))
        {
            Console.WriteLine();
            Console.WriteLine("record sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):");
            Print(con, tx, "select cast(lpad('', level * 2) || cast(access_path as varchar(120)) "
                           + "           as varchar(140)) as access_path, "
                           + "       open_counter as opens, fetch_counter as fetches, "
                           + "       open_fetch_total_elapsed_time as total_ns "
                           + "from plg$profiler.plg$prof_record_source_stats_view "
                           + "where profile_id = @id and sql_text containing 'join nums' "
                           + "order by cursor_id, record_source_id", profileId);

            Console.WriteLine();
            Console.WriteLine("hotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):");
            Print(con, tx, "select line_num, column_num, counter, "
                           + "       total_elapsed_time as total_ns, avg_elapsed_time as avg_ns "
                           + "from plg$profiler.plg$prof_psql_stats_view "
                           + "where profile_id = @id and routine_name = 'HOTSPOT' "
                           + "order by total_elapsed_time desc", profileId);
            tx.Commit();
        }
        Console.WriteLine("done.");
    }
}
