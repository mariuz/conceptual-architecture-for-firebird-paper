//
// Schemas.cs - SQL schemas and the search path (C# twin of
// ../../cpp/schemas.cpp; see ../../../schemas-and-name-resolution.md).
//
// The same five demonstrations: RDB$SCHEMAS and the default path, one
// unqualified SELECT changing meaning as SET SEARCH_PATH changes, the
// SYSTEM auto-append, a procedure that keeps binding its own schema after
// the session's path flips, and the plan naming the resolved table
// (FbCommand.GetCommandPlan(), plus GetCommandExplainedPlan()).  The
// instructive difference is ADO.NET's own metadata surface,
// DbConnection.GetSchema("Tables"): FirebirdClient 10 predates Firebird 6
// schemas, so its TABLE_SCHEMA column is NULL and both CUSTOMERS tables
// come back as two indistinguishable rows - the engine resolves names by
// schema while the provider's metadata layer cannot tell them apart (the
// same gap as Jaybird 6's DatabaseMetaData).
//
// Run:  cd samples/csharp && dotnet run -- Schemas [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Schemas
{
    static object? SearchPath(FbConnection con) =>
        Scalar(con, "select rdb$get_context('SYSTEM', 'SEARCH_PATH') from rdb$database");

    static object? Origin(FbConnection con) => Scalar(con, "select origin from customers");

    public static void Run(string[] args)
    {
        var path = DbPath("schemas", args);
        using var con = AttachOrCreate(path);

        // Idempotent cleanup + setup (no explicit transaction: each statement auto-commits).
        foreach (var s in new[] { "drop procedure app.which_one", "drop table public.customers",
                                  "drop table app.customers", "drop schema app" })
            try { Execute(con, s); } catch (FbException) { /* did not exist */ }
        Execute(con, "create schema app");
        Execute(con, "create table public.customers (id int, origin varchar(20))");
        Execute(con, "create table app.customers    (id int, origin varchar(20))");
        Execute(con, "insert into public.customers values (1, 'from PUBLIC')");
        Execute(con, "insert into app.customers    values (2, 'from APP')");

        // 1. The catalog and the default path.
        var names = Rows(con, "select trim(rdb$schema_name) from rdb$schemas order by 1").Select(r => Text(r[0]));
        Console.WriteLine($"schemas in RDB$SCHEMAS      : {string.Join("  ", names)}");
        Console.WriteLine($"default search path         : {SearchPath(con)}");

        // 2. Same statement, two resolutions.
        Console.WriteLine();
        Console.WriteLine("SELECT ORIGIN FROM CUSTOMERS, as the path changes:");
        Console.WriteLine($"  path PUBLIC,SYSTEM        -> {Origin(con)}");
        Execute(con, "set search_path to app, public");
        Console.WriteLine($"  path APP,PUBLIC           -> {Origin(con)}");

        // 3. SYSTEM can be moved but not removed.
        Execute(con, "set search_path to app");
        Console.WriteLine();
        Console.WriteLine($"SET SEARCH_PATH TO APP      -> {SearchPath(con)}   (SYSTEM auto-appended)");

        // 4. Stored code binds its own schema, not the caller's path.
        Execute(con, "set search_path to app, public");
        Execute(con, "create procedure which_one returns (src varchar(20)) as "
                     + "begin select origin from customers into :src; suspend; end");
        Console.WriteLine();
        Console.WriteLine("procedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)");
        Execute(con, "set search_path to public");
        Console.WriteLine("  after SET SEARCH_PATH TO PUBLIC:");
        Console.WriteLine($"    direct SELECT ... FROM CUSTOMERS -> {Origin(con)}");
        Console.WriteLine($"    SELECT SRC FROM APP.WHICH_ONE    -> {Scalar(con, "select src from app.which_one")}   <- unmoved");
        Console.WriteLine("    RDB$DEPENDENCIES records         -> " + Scalar(con,
            "select trim(rdb$depended_on_schema_name) || '.' || trim(rdb$depended_on_name)"
            + " from rdb$dependencies where rdb$dependent_name = 'WHICH_ONE'"));

        // 5. Plans are schema-qualified (prepared, never executed).
        using (var tx = con.BeginTransaction())
        using (var cmd = new FbCommand("select count(*) from customers", con, tx))
        {
            cmd.Prepare();
            Console.WriteLine();
            Console.WriteLine($"plan for unqualified SELECT : {cmd.GetCommandPlan().Trim()}");
            Console.WriteLine("explained plan              : "
                              + cmd.GetCommandExplainedPlan().Trim().Replace("\n", "\n                              "));
            tx.Commit();
        }

        // 6. ...and what ADO.NET's metadata layer makes of it.
        var tables = con.GetSchema("Tables", new[] { null, null, "CUSTOMERS" });
        var cols = tables.Columns.Cast<System.Data.DataColumn>().Select(c => c.ColumnName).ToList();
        Console.WriteLine();
        Console.WriteLine($"ADO.NET GetSchema(\"Tables\") (FirebirdClient {typeof(FbConnection).Assembly.GetName().Version}):");
        Console.WriteLine($"  columns: {string.Join(", ", cols.Take(4))}, ...");
        foreach (System.Data.DataRow r in tables.Rows)
            Console.WriteLine($"  -> TABLE_SCHEMA={Text(r["TABLE_SCHEMA"])} TABLE_NAME={Text(r["TABLE_NAME"])}");

        Console.WriteLine();
        Console.WriteLine("done.");
    }
}
