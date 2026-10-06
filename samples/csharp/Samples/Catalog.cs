//
// Catalog.cs - the catalog describing itself, from client SQL (C# twin of
// ../../cpp/catalog.cpp; see ../../../catalog-bootstrap.md).
//
// On a freshly recreated database: (1) the fixed relation ids of
// relations.h (RDB$PAGES 0, RDB$DATABASE 1, RDB$FIELDS 2, RDB$RELATIONS 6);
// (2) RDB$PAGES carrying its own pointer page, cross-checked against the
// hdr_PAGES word at byte 28 of page 0 - read with a FileStream and
// BinaryPrimitives, below any driver (with the System.IO.DisableFileLocking
// switch on: .NET's Unix flock() would collide with the server's); (3) RDB$FORMATS empty while the
// system relations are fully usable (their formats are compiled into the
// engine); (4) user DDL planting the first RDB$FORMATS rows.  The ADO.NET
// addition: GetSchema("Tables") - the portable catalog API - filtered by
// its TABLE_TYPE restriction classifies the same relations as SYSTEM TABLE
// or TABLE from RDB$SYSTEM_FLAG.  Run it on the server machine.
//
// Run:  cd samples/csharp && dotnet run -- Catalog [database]
//
using System.Buffers.Binary;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Catalog
{
    /// <summary>isql-style listing; headers from the reader unless given.</summary>
    static void Table(FbConnection con, string sql, params string[] headers)
    {
        using var cmd = new FbCommand(sql, con);
        using var r = cmd.ExecuteReader();
        var names = headers.Length > 0 ? headers : Enumerable.Range(0, r.FieldCount).Select(r.GetName).ToArray();
        var rows = new List<string[]>();
        while (r.Read())
            rows.Add(Enumerable.Range(0, r.FieldCount).Select(i => Text(r.GetValue(i)).Trim()).ToArray());
        var w = names.Select((n, i) => rows.Select(x => x[i].Length).Prepend(n.Length).Max()).ToArray();
        string Line(IEnumerable<string> cells) =>
            string.Join(" ", cells.Select((c, i) => c.PadRight(w[i]))).TrimEnd();
        Console.WriteLine(Line(names));
        Console.WriteLine(Line(w.Select(n => new string('-', n))));
        foreach (var row in rows)
            Console.WriteLine(Line(row));
    }

    /// <summary>The hdr_PAGES word: little-endian uint32 at byte 28 of page 0.</summary>
    static uint HdrPages(string file)
    {
        // .NET on Unix takes an advisory flock() on every FileStream, and the
        // server holds an exclusive one on the live database; opt out (the
        // same trap fbintf's TFileStream hits).
        AppContext.SetSwitch("System.IO.DisableFileLocking", true);
        using var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        Span<byte> b = stackalloc byte[4];
        f.Seek(28, SeekOrigin.Begin);
        f.ReadExactly(b);
        return BinaryPrimitives.ReadUInt32LittleEndian(b);
    }

    static int CountTables(FbConnection con, string type) =>
        con.GetSchema("Tables", new[] { null, null, null, type }).Rows.Count;

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
        var path = DbPath("catalog", args);
        using var con = Fresh(path);

        Console.WriteLine("-- 1. fixed relation ids (relations.h declaration order) --");
        Table(con, "select rdb$relation_id, trim(rdb$relation_name) "
                 + "from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1", "ID", "NAME");

        Console.WriteLine("\n-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --");
        Table(con, "select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type "
                 + "from rdb$pages where rdb$relation_id in (0, 6) "
                 + "order by rdb$relation_id, rdb$page_type, rdb$page_number");
        Console.WriteLine($"\nhdr_PAGES (page 0, offset 28) = {HdrPages(path)}"
                          + "  <- matches the (relation 0, type 4) row above");

        Console.WriteLine("\n-- 3. formats as code: zero stored formats, yet a full catalog --");
        Table(con, "select (select count(*) from rdb$formats), "
                 + "       (select count(*) from rdb$relations where rdb$system_flag = 1), "
                 + "       (select count(*) from rdb$relation_fields r join rdb$relations rel "
                 + "          on r.rdb$relation_name = rel.rdb$relation_name "
                 + "          and r.rdb$schema_name = rel.rdb$schema_name "
                 + "        where rel.rdb$system_flag = 1) "
                 + "from rdb$database", "FORMATS_ROWS", "SYS_RELATIONS", "SYS_FIELDS");
        Console.WriteLine($"GetSchema(\"Tables\"): {CountTables(con, "SYSTEM TABLE")} SYSTEM TABLE, "
                          + $"{CountTables(con, "TABLE")} TABLE");

        Console.WriteLine("\n-- 4. user DDL writes formats into the catalog --");
        Execute(con, "create table t1 (a integer)");
        Execute(con, "alter table t1 add b varchar(10)");
        Table(con, "select rdb$relation_id, rdb$format, octet_length(rdb$descriptor) "
                 + "from rdb$formats order by rdb$relation_id, rdb$format",
              "RDB$RELATION_ID", "RDB$FORMAT", "DESCRIPTOR_BYTES");
        Console.WriteLine("\n(relation id of T1: "
                          + Scalar(con, "select rdb$relation_id from rdb$relations where rdb$relation_name = 'T1'")
                          + " - the first user id; system tables still contribute no rows)");
        Console.WriteLine($"GetSchema(\"Tables\"): {CountTables(con, "SYSTEM TABLE")} SYSTEM TABLE, "
                          + $"{CountTables(con, "TABLE")} TABLE");
        Console.WriteLine("done.");
    }
}
