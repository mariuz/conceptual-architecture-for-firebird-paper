//
// Indexes.cs - one B-tree, many variants (C# twin of ../../cpp/indexes.cpp;
// see ../../../indexing-and-full-text-search.md).
//
// A 3,000-row table gets a descending, an expression (COMPUTED BY), a
// partial (WHERE) and a plain index; five queries are prepared and their
// plans printed: expression index, partial index, descending navigation,
// a two-index bitmap OR, and CONTAINING falling to NATURAL.  The provider
// asks the *statement*, as the C++ sample does: FbCommand.GetCommandPlan()
// is the legacy one-line PLAN and GetCommandExplainedPlan() the explained
// tree (isc_info_sql_get_plan / isc_info_sql_explain_plan in the prepare
// info request, sent over the managed wire protocol).  The ADO.NET
// addition is GetSchema("Indexes") / GetSchema("IndexColumns"), the
// portable index catalog, which shows what a generic tool can and cannot
// see of the variants: the descending one only as INDEX_TYPE = 1, the
// expression index with a <null> column, the partial index's WHERE
// nowhere (no collection column carries RDB$CONDITION_SOURCE).
//
// Run:  cd samples/csharp && dotnet run -- Indexes [database]
//
using System.Data;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Indexes
{
    static void Plan(FbConnection con, string sql, bool explained = false)
    {
        using var cmd = new FbCommand(sql, con);
        cmd.Prepare();
        Console.WriteLine(sql);
        Console.WriteLine(cmd.GetCommandPlan().Trim());
        if (explained)
        {
            Console.WriteLine("explained:");
            Console.WriteLine("  " + cmd.GetCommandExplainedPlan().Trim().Replace("\n", "\n  "));
        }
        Console.WriteLine();
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("indexes", args));
        Execute(con, "recreate table doc ("
                   + " id integer, title varchar(60), status varchar(10), num integer)");
        Execute(con, "execute block as declare i integer = 0; begin"
                   + "  while (i < 3000) do begin"
                   + "    insert into doc values (:i, 'Title ' || :i,"
                   + "      iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100));"
                   + "    i = i + 1;"
                   + "  end "
                   + "end");
        Execute(con, "create descending index doc_id_desc on doc (id)");
        Execute(con, "create index doc_upper_title on doc computed by (upper(title))");
        Execute(con, "create index doc_active on doc (status) where status = 'active'");
        Execute(con, "create index doc_num on doc (num)");
        Console.WriteLine("3000 rows; indexes: descending, expression, partial, plain\n");

        Plan(con, "select id from doc where upper(title) = 'TITLE 5'");
        Plan(con, "select id from doc where status = 'active'");
        Plan(con, "select first 1 id from doc order by id desc");
        Plan(con, "select id from doc where num = 42 or id = 7", explained: true);
        Plan(con, "select id from doc where title containing 'itle 12'");

        Console.WriteLine("CONTAINING is correct but unindexed: matched "
                          + Scalar(con, "select count(*) from doc where title containing 'itle 12'")
                          + " rows by scanning all 3000\n");

        // The portable view: what ADO.NET's schema collections report.
        Console.WriteLine("GetSchema(\"Indexes\") + GetSchema(\"IndexColumns\") for DOC:");
        var restrict = new[] { null, null, "DOC" };
        var indexes = con.GetSchema("Indexes", restrict);
        var columns = con.GetSchema("IndexColumns", restrict);
        foreach (DataRow ix in indexes.Rows.Cast<DataRow>().OrderBy(r => Text(r["INDEX_NAME"])))
        {
            var name = Text(ix["INDEX_NAME"]).Trim();
            var cols = columns.Rows.Cast<DataRow>()
                .Where(c => Text(c["INDEX_NAME"]).Trim() == name)
                .Select(c => Text(c["COLUMN_NAME"]).Trim()).ToList();
            Console.WriteLine($"  {name,-16} column={string.Join(", ", cols),-8}"
                              + $" INDEX_TYPE={Text(ix["INDEX_TYPE"])}");   // RDB$INDEX_TYPE: 1 = descending
        }
        Console.WriteLine("  (collection columns: " + string.Join(", ",
            indexes.Columns.Cast<DataColumn>().Select(c => c.ColumnName)) + ")");
        Console.WriteLine("done.");
    }
}
