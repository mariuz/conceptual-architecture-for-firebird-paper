//
// ParserErrors.cs - driving Firebird's SQL parser from the client (C# twin
// of ../../cpp/parser_errors.cpp; see ../../../grammar-and-parser.md).
//
// Six strings are prepared against the stock employee database: a `?`
// placeholder, FIRST in its two grammatical roles (row-limit clause and
// plain column name), two syntax errors with token line/column, and a
// semantic error whose position survives the parse.  FbCommand.Prepare()
// is a genuine prepare-only step (op_prepare_statement, nothing executes),
// and ExecuteReader(CommandBehavior.SchemaOnly) hands back the prepared
// statement's output descriptors as a schema table - still without
// executing.  What the provider does not publish is the input side: the
// `?` descriptor and the isc_info_sql_stmt_* type stay internal, so the
// C++ run's `sqltype=500` is out of reach here.  The error channel is
// whole: FbException.Errors keeps one FbError per status-vector item - gds
// codes *and* numeric arguments - so the sample tells
// isc_dsql_token_unk_err (syntax) from isc_dsql_field_err (semantic) and
// reads the line/column as numbers, not out of the message text.
//
// Run:  cd samples/csharp && dotnet run -- ParserErrors
//
using System.Data;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class ParserErrors
{
    const int IscDsqlFieldErr = 335544578;     // isc_dsql_field_err: Column unknown
    const int IscDsqlTokenUnkErr = 335544634;  // isc_dsql_token_unk_err: Token unknown
    const int IscDsqlLineColError = 336397208; // isc_dsql_line_col_error: At line N, column M

    /// <summary>Feed one string to the parser; report the statement's shape or the status vector.</summary>
    static void TryPrepare(FbConnection con, FbTransaction tx, string sql)
    {
        Console.WriteLine("---- " + sql);
        try
        {
            using var cmd = new FbCommand(sql, con, tx);
            if (sql.Contains('?'))
                cmd.Parameters.Add(new FbParameter());   // a slot for the `?`; never given a value
            cmd.Prepare();
            using var r = cmd.ExecuteReader(CommandBehavior.SchemaOnly);
            var schema = r.GetSchemaTable();
            Console.WriteLine($"  parsed OK: bound slots={cmd.Parameters.Count}, output columns={schema.Rows.Count}");
            foreach (DataRow col in schema.Rows)
                Console.WriteLine($"    column {col["ColumnOrdinal"]}: {col["ColumnName"]} "
                                  + $"{((FbDbType)col["ProviderType"])} size={col["ColumnSize"]}");
        }
        catch (FbException e)
        {
            // Errors is the status vector item by item: gds codes, and the
            // isc_arg_number arguments as bare Numbers (string arguments come
            // through as 0); the provider appends one last entry carrying
            // the formatted message.
            var items = e.Errors.Select(x => x.Number).ToList();
            var kind = items.Contains(IscDsqlTokenUnkErr) ? "syntax"
                     : items.Contains(IscDsqlFieldErr) ? "semantic" : "?";
            Console.WriteLine($"  prepare failed ({kind}; SQLSTATE {e.SQLSTATE}, gds {e.ErrorCode}):");
            Console.WriteLine(ErrorText(e));
            Console.WriteLine("  Errors[].Number: " + string.Join(" ", items));
            // The token position rides as two numeric arguments after the
            // code that reports it - readable without parsing the text.
            var at = items.FindIndex(n => n is IscDsqlTokenUnkErr or IscDsqlLineColError);
            if (at >= 0 && at + 2 < items.Count)
                Console.WriteLine($"  position from the vector: line {items[at + 1]}, column {items[at + 2]}");
        }
    }

    public static void Run(string[] args)
    {
        using var con = Employee();
        using var tx = con.BeginTransaction();

        // 1. Dynamic SQL: the `?` becomes a typed parameter.
        TryPrepare(con, tx, "SELECT first_name FROM employee WHERE emp_no = ?");

        // 2. One token, two grammatical roles: FIRST as row-limit clause...
        TryPrepare(con, tx, "SELECT FIRST 1 emp_no FROM employee");
        // ...and FIRST as an ordinary identifier (non-reserved keyword).
        TryPrepare(con, tx, "SELECT first FROM (SELECT 1 AS first FROM rdb$database)");

        // 3. Syntax errors with token position.
        TryPrepare(con, tx, "SELEC 1 FROM rdb$database");
        TryPrepare(con, tx, "SELECT emp_no\nFROM employee\nWHERE ORDER BY 1");

        // 4. Semantic error - still carries line/column.
        TryPrepare(con, tx, "SELECT frst_name\nFROM employee");

        tx.Commit();
        Console.WriteLine("done.");
    }
}
