//
// Windows.java - window functions and modern aggregates (Java twin of
// ../../../../../cpp/windows.cpp; see
// ../../../../../../aggregate-and-window-functions.md).
//
// Recreates the document's six-row sales table and runs its flagship
// analytics: the ranking / framed running total / LAG window query,
// FILTER + LISTAGG + STDDEV_POP, PERCENTILE_CONT and a hypothetical-set
// RANK(175) WITHIN GROUP, and Firebird 6's EXCLUDE CURRENT ROW frame.
//
// Window functions are plain SQL, so the differences are in the driver:
// every NUMERIC arrives as an exact java.math.BigDecimal (the INT128-wide
// SUM included, no CAST needed), PERCENTILE_CONT is a DOUBLE and so a
// Java double.  And unlike the Go and Rust twins, Jaybird has a plan API:
// FirebirdPreparedStatement.getExecutionPlan() returns the legacy plan the
// C++ sample prints with IStatement::getPlan(false), and
// getExplainedExecutionPlan() the structured one - no RDB$SQL.EXPLAIN.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Windows
//
package fbsamples;

import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

import org.firebirdsql.jdbc.FirebirdPreparedStatement;

public final class Windows {

    /** Print a query as an aligned table headed by its column labels. */
    static void print(Connection con, String sql) throws SQLException {
        List<String[]> table = new ArrayList<>();
        try (Statement st = con.createStatement(); ResultSet rs = st.executeQuery(sql)) {
            ResultSetMetaData md = rs.getMetaData();
            int n = md.getColumnCount();
            String[] head = new String[n];
            for (int i = 0; i < n; i++) {
                head[i] = md.getColumnLabel(i + 1);
            }
            table.add(head);
            while (rs.next()) {
                String[] row = new String[n];
                for (int i = 0; i < n; i++) {
                    row[i] = FbSample.text(rs.getObject(i + 1));
                }
                table.add(row);
            }
        }
        int n = table.get(0).length;
        int[] w = new int[n];
        for (String[] r : table) {
            for (int i = 0; i < n; i++) {
                w[i] = Math.max(w[i], r[i].length());
            }
        }
        for (int r = 0; r < table.size(); r++) {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < n; i++) {
                sb.append(String.format("%-" + w[i] + "s ", table.get(r)[i]));
            }
            System.out.println(sb.toString().stripTrailing());
            if (r == 0) {
                StringBuilder dash = new StringBuilder();
                for (int i = 0; i < n; i++) {
                    dash.append("-".repeat(w[i])).append(' ');
                }
                System.out.println(dash.toString().stripTrailing());
            }
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            try (Connection con = FbSample.attachOrCreate(dbPath("windows", args))) {
                execute(con, "recreate table sales (id int primary key, region varchar(10),"
                        + " amount numeric(10,2))");
                con.setAutoCommit(false);
                try (PreparedStatement ins = con.prepareStatement("insert into sales values (?, ?, ?)")) {
                    Object[][] rows = {
                        {1, "East", 100}, {2, "East", 200}, {3, "East", 150},
                        {4, "West", 300}, {5, "West", 250}, {6, "West", 400},
                    };
                    for (Object[] r : rows) {
                        ins.setInt(1, (Integer) r[0]);
                        ins.setString(2, (String) r[1]);
                        ins.setBigDecimal(3, java.math.BigDecimal.valueOf((Integer) r[2]));
                        ins.addBatch();
                    }
                    ins.executeBatch();
                }

                // -- 1. partitioned ranking, framed running total, LAG
                String winSql = "select region, amount,"
                        + " row_number() over (partition by region order by amount) as rn,"
                        + " rank() over (order by amount desc) as overall_rank,"
                        + " sum(amount) over (partition by region order by id"
                        + "   rows between unbounded preceding and current row) as running_total,"
                        + " lag(amount) over (partition by region order by id) as prev_amount"
                        + " from sales";
                System.out.println("== window functions ==");
                print(con, winSql);
                try (PreparedStatement ps = con.prepareStatement(winSql)) {
                    FirebirdPreparedStatement fps = ps.unwrap(FirebirdPreparedStatement.class);
                    System.out.println();
                    System.out.println("plan:" + fps.getExecutionPlan());
                    System.out.println();
                    System.out.println("explained plan:" + fps.getExplainedExecutionPlan());
                }

                // -- 2. FILTER, ordered LISTAGG, statistical aggregate
                System.out.println();
                System.out.println("== aggregates: FILTER / LISTAGG / STDDEV_POP ==");
                print(con, "select region, count(*) as n,"
                        + " count(*) filter (where amount > 150) as big_sales,"
                        + " cast(listagg(amount, ',') within group (order by amount)"
                        + "   as varchar(60)) as amounts,"
                        + " cast(stddev_pop(amount) as numeric(10,2)) as stddev"
                        + " from sales group by region");

                // -- 3. ordered-set and hypothetical-set aggregates
                System.out.println();
                System.out.println("== PERCENTILE_CONT median / hypothetical RANK(175) ==");
                print(con, "select region,"
                        + " percentile_cont(0.5) within group (order by amount) as median,"
                        + " rank(175) within group (order by amount) as rank_of_175"
                        + " from sales group by region");

                // -- 4. Firebird 6 frame exclusion
                System.out.println();
                System.out.println("== FB6 frame EXCLUDE CURRENT ROW (neighbours' average) ==");
                print(con, "select id, amount,"
                        + " cast(avg(amount) over (order by id"
                        + "   rows between 1 preceding and 1 following"
                        + "   exclude current row) as numeric(10,2)) as neighbour_avg"
                        + " from sales");

                con.commit();
            }
            System.out.println();
            System.out.println("done.");
        });
    }
}
