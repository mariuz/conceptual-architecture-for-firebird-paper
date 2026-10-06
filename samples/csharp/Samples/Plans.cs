//
// Plans.cs - watching the cost-based optimizer decide (C# twin of
// ../../cpp/plans.cpp; see ../../../query-optimizer-and-execution.md).
//
// Every statement is prepared, never executed, and both plan forms are
// printed: the terse legacy PLAN (...) and the detailed record-source tree.
// The same SELECT is prepared before and after CREATE INDEX (Full Scan ->
// Bitmap + Index Range Scan); then a unique-key lookup, SORT over a nested
// loop, and the Firebird 5 hash join for an indexless equi-join.
//
// FirebirdClient asks for the plan info items itself, over its managed
// wire protocol: after FbCommand.Prepare(), GetCommandPlan() sends
// isc_info_sql_get_plan and GetCommandExplainedPlan() sends
// isc_info_sql_explain_plan in an op_info_sql - no RDB$SQL.EXPLAIN detour,
// no libfbclient.  Both are provider extensions on FbCommand; ADO.NET
// itself has no notion of a plan.
//
// Run:  cd samples/csharp && dotnet run -- Plans [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Plans
{
    static void ShowPlan(FbConnection con, string sql)
    {
        Console.WriteLine("== " + sql);
        using var tx = con.BeginTransaction();
        using var cmd = new FbCommand(sql, con, tx);
        cmd.Prepare();                                           // server-side prepare, no execute
        Console.WriteLine("legacy:  " + cmd.GetCommandPlan().Trim());
        Console.WriteLine("detailed:");
        Console.WriteLine(cmd.GetCommandExplainedPlan().Trim());
        tx.Rollback();
        Console.WriteLine();
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("plans", args));

        // -- Schema: 20 departments, 2000 employees (auto-commit). ---------------
        Execute(con, "recreate table emp (id int not null primary key,"
                     + " dept_id int, salary int, name varchar(20))");
        Execute(con, "recreate table dept (id int not null primary key, name varchar(20))");
        Execute(con, """
            EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN
              WHILE (i <= 20) DO BEGIN
                INSERT INTO dept VALUES (:i, 'dept ' || :i); i = i + 1;
              END
              i = 1;
              WHILE (i <= 2000) DO BEGIN
                INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,
                    1000 + MOD(:i * 37, 500), 'emp ' || :i); i = i + 1;
              END
            END
            """);

        // -- 1. No index on dept_id yet: the full scan is the only path. ---------
        ShowPlan(con, "SELECT name FROM emp WHERE dept_id = 5");

        // -- 2. Create the index; the same text now compiles differently. --------
        Execute(con, "CREATE INDEX emp_dept ON emp (dept_id)");
        Console.WriteLine("-- CREATE INDEX emp_dept ON emp (dept_id) --");
        Console.WriteLine();
        ShowPlan(con, "SELECT name FROM emp WHERE dept_id = 5");

        // -- 3. PK equality: unique index, nothing cheaper than one row. ---------
        ShowPlan(con, "SELECT name FROM emp WHERE id = 42");

        // -- 4. Join + ORDER BY: SORT over a nested loop with the index. ---------
        ShowPlan(con, "SELECT e.name, d.name FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.salary");

        // -- 5. Equi-join with no usable index on either side: hash join. --------
        ShowPlan(con, "SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary");
        Console.WriteLine("done.");
    }
}
