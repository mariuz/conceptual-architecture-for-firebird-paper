//
// Plans.java - watching the cost-based optimizer decide (Java twin of
// ../../../../../cpp/plans.cpp; see
// ../../../../../../query-optimizer-and-execution.md).
//
// Every statement is prepared, never executed, and both plan forms are
// printed: the terse legacy PLAN (...) and the detailed record-source tree.
// The same SELECT is prepared before and after CREATE INDEX (Full Scan ->
// Bitmap + Index Range Scan); then a unique-key lookup, SORT over a nested
// loop, and the Firebird 5 hash join for an indexless equi-join.
//
// Jaybird is the pure wire-protocol driver that does request the plan info
// items: FirebirdPreparedStatement.getExecutionPlan() sends
// isc_info_sql_get_plan and getExplainedExecutionPlan() sends
// isc_info_sql_explain_plan over op_info_sql.  So, unlike node-firebird,
// rsfbclient and the Go driver - which detour through RDB$SQL.EXPLAIN - it
// prints the legacy one-liner and the tree straight from a prepared
// statement, like the C++ and Python twins.  Connection.prepareStatement
// prepares on the server at once; nothing is executed.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Plans
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.SQLException;

import org.firebirdsql.jdbc.FirebirdPreparedStatement;

public final class Plans {

    private static void showPlan(Connection con, String sql) throws SQLException {
        System.out.println("== " + sql);
        try (PreparedStatement ps = con.prepareStatement(sql)) {
            FirebirdPreparedStatement fps = ps.unwrap(FirebirdPreparedStatement.class);
            System.out.println("legacy:  " + fps.getExecutionPlan().strip());
            System.out.println("detailed:");
            System.out.println(fps.getExplainedExecutionPlan().strip());
        }
        System.out.println();
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("plans", args);
            try (Connection con = attachOrCreate(path)) {
                // -- Schema: 20 departments, 2000 employees (auto-commit). ------
                execute(con, "recreate table emp (id int not null primary key,"
                        + " dept_id int, salary int, name varchar(20))");
                execute(con, "recreate table dept (id int not null primary key, name varchar(20))");
                execute(con, """
                        EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN
                          WHILE (i <= 20) DO BEGIN
                            INSERT INTO dept VALUES (:i, 'dept ' || :i); i = i + 1;
                          END
                          i = 1;
                          WHILE (i <= 2000) DO BEGIN
                            INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,
                                1000 + MOD(:i * 37, 500), 'emp ' || :i); i = i + 1;
                          END
                        END""");

                // -- 1. No index on dept_id yet: the full scan is the only path.
                showPlan(con, "SELECT name FROM emp WHERE dept_id = 5");

                // -- 2. Create the index; the same text now compiles differently.
                execute(con, "CREATE INDEX emp_dept ON emp (dept_id)");
                System.out.println("-- CREATE INDEX emp_dept ON emp (dept_id) --");
                System.out.println();
                showPlan(con, "SELECT name FROM emp WHERE dept_id = 5");

                // -- 3. PK equality: unique index, nothing cheaper than one row.
                showPlan(con, "SELECT name FROM emp WHERE id = 42");

                // -- 4. Join + ORDER BY: SORT over a nested loop with the index.
                showPlan(con, "SELECT e.name, d.name FROM emp e"
                        + " JOIN dept d ON e.dept_id = d.id ORDER BY e.salary");

                // -- 5. Equi-join with no usable index on either side: hash join.
                showPlan(con, "SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary");
            }
            System.out.println("done.");
        });
    }
}
