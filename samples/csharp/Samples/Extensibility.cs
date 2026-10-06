//
// Extensibility.cs - calling native code through SQL: UDR end to end
// (C# twin of ../../cpp/extensibility.cpp; see ../../../extensibility.md).
//
// The shipped example UDR module (plugins/udr/libudrcpp_example.so) is bound
// to SQL names with EXTERNAL NAME '<module>!<entry>' ENGINE udr and called
// like any other procedure/function; then RDB$PROCEDURES / RDB$FUNCTIONS
// show the binding as metadata and RDB$CONFIG names the plugin filling each
// role.  Every seam lives on the server, so the provider's managed wire
// protocol loses nothing.  The ADO.NET idiom adds named @parameters (the
// C++ sample inlines literals) and a contrast: DbConnection.GetSchema - the
// provider's portable catalog API - lists both routines, and its
// "Functions" collection even carries RDB$ENTRYPOINT (as the legacy-UDF
// column FUNCTION_ENTRY_POINT), but "Procedures" has no such column and
// neither names the engine: telling a UDR from PSQL still takes RDB$.
//
// Run:  cd samples/csharp && dotnet run -- Extensibility [database]
//
using System.Data;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Extensibility
{
    /// <summary>isql-style listing: headers from the reader's column names.</summary>
    static void Table(FbDataReader r)
    {
        var names = Enumerable.Range(0, r.FieldCount).Select(r.GetName).ToArray();
        var rows = new List<string[]>();
        while (r.Read())
            rows.Add(Enumerable.Range(0, r.FieldCount)
                .Select(i => r.IsDBNull(i) ? "" : Text(r.GetValue(i)).Trim()).ToArray());
        var w = names.Select((n, i) => rows.Select(x => x[i].Length).Prepend(n.Length).Max()).ToArray();
        string Line(string[] cells) =>
            string.Join(" ", cells.Select((c, i) => c.PadRight(w[i]))).TrimEnd();
        Console.WriteLine(Line(names));
        Console.WriteLine(Line(w.Select(n => new string('-', n)).ToArray()));
        foreach (var row in rows)
            Console.WriteLine(Line(row));
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("extensibility", args));

        // 1. Bind SQL names to entry points in the shipped native module.
        Execute(con, "recreate procedure gen_rows (start_n integer not null, "
                   + "                             end_n integer not null) "
                   + "  returns (n integer not null) "
                   + "  external name 'udrcpp_example!gen_rows' engine udr");
        Execute(con, "recreate function sum_args (n1 integer, n2 integer, n3 integer) "
                   + "  returns integer "
                   + "  external name 'udrcpp_example!sum_args' engine udr");

        // 2. Call them: native C++ running inside the server, @parameters here.
        Console.WriteLine("select n from gen_rows(@start_n, @end_n)  [1, 5]:");
        using (var cmd = new FbCommand("select n from gen_rows(@start_n, @end_n)", con))
        {
            cmd.Parameters.Add("@start_n", FbDbType.Integer).Value = 1;
            cmd.Parameters.Add("@end_n", FbDbType.Integer).Value = 5;
            using var r = cmd.ExecuteReader();
            Table(r);
        }
        using (var cmd = new FbCommand("select sum_args(@n1, @n2, @n3) from rdb$database", con))
        {
            cmd.Parameters.AddWithValue("@n1", 19);
            cmd.Parameters.AddWithValue("@n2", 20);
            cmd.Parameters.AddWithValue("@n3", 3);
            Console.WriteLine($"\nselect sum_args(19, 20, 3):  {cmd.ExecuteScalar()}");
        }

        // 3. The binding is ordinary metadata...
        Console.WriteLine("\nexternal routines recorded in the system tables:");
        foreach (var row in Rows(con,
                     "select trim(rdb$procedure_name) || '  ->  ' || "
                   + "       trim(rdb$entrypoint) || '  (engine ' || "
                   + "       trim(rdb$engine_name) || ')' "
                   + "from rdb$procedures where rdb$engine_name = 'UDR' "
                   + "union all "
                   + "select trim(rdb$function_name) || '  ->  ' || "
                   + "       trim(rdb$entrypoint) || '  (engine ' || "
                   + "       trim(rdb$engine_name) || ')' "
                   + "from rdb$functions where rdb$engine_name = 'UDR'"))
            Console.WriteLine(Text(row[0]));

        // ...which ADO.NET's portable catalog API (GetSchema) flattens away.
        Console.WriteLine("\nthe same routines via GetSchema:");
        foreach (var (collection, name, columns) in new[]
                 {
                     ("Procedures", "GEN_ROWS", new[] { "PROCEDURE_NAME", "INPUTS", "OUTPUTS", "SOURCE" }),
                     ("Functions", "SUM_ARGS", new[] { "FUNCTION_NAME", "FUNCTION_TYPE", "FUNCTION_MODULE_NAME", "FUNCTION_ENTRY_POINT" }),
                 })
        {
            var table = con.GetSchema(collection, new[] { null, null, name });
            Console.WriteLine($"GetSchema(\"{collection}\"), {table.Columns.Count} columns, of which:");
            foreach (DataRow row in table.Rows)
                foreach (var c in columns)
                    Console.WriteLine($"    {c,-22} {Text(row[c]).Trim()}");
        }

        // 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG.
        Console.WriteLine("\nplugins filling each role (rdb$config):");
        using (var cmd = new FbCommand(
                   "select rdb$config_name, rdb$config_value "
                 + "from rdb$config "
                 + "where rdb$config_name in ('Providers', 'AuthServer', "
                 + "      'UserManager', 'WireCryptPlugin', 'TracePlugin', "
                 + "      'DefaultProfilerPlugin') "
                 + "order by rdb$config_id", con))
        using (var r = cmd.ExecuteReader())
            Table(r);

        Console.WriteLine("\ndone.");
    }
}
