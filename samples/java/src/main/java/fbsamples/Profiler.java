//
// Profiler.java - the profiler's accumulation view, driven from client code
// (Java twin of ../../../../../cpp/profiler.cpp; see
// ../../../../../../profiler.md).
//
// One RDB$PROFILER session brackets two workloads - a self-join over a
// 5,000-row table and a 20,000-iteration PSQL loop - and the PLG$PROFILER
// schema is then queried like any other data: the record-source view as an
// indented plan tree, the PSQL view ranked by time per line and column.
//
// JDBC, like every other driver, loses nothing here: the control surface is
// a SQL package and the output a SQL schema.  The Java-specific details:
// the procedure is a text block, which strips the common indentation but
// keeps the relative one, so the column numbers the profiler reports are
// real; and the session runs inside an explicit SNAPSHOT transaction
// (auto-commit off, TRANSACTION_REPEATABLE_READ) so the sample can show the
// autonomous-flush pitfall - a count taken in that same transaction after
// FINISH_SESSION(TRUE) sees nothing - before a real commit() makes the
// flush visible.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Profiler
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;
import static fbsamples.FbSample.text;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;

public final class Profiler {

    private static final String HOTSPOT = """
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
            end""";

    /** Print a query with a bound profile id as an aligned table. */
    private static void print(Connection con, String sql, long profileId) throws SQLException {
        try (PreparedStatement ps = con.prepareStatement(sql)) {
            ps.setLong(1, profileId);
            try (ResultSet rs = ps.executeQuery()) {
                ResultSetMetaData md = rs.getMetaData();
                int n = md.getColumnCount();
                List<String[]> table = new ArrayList<>();
                String[] head = new String[n];
                for (int i = 0; i < n; i++) {
                    head[i] = md.getColumnLabel(i + 1);
                }
                table.add(head);
                while (rs.next()) {
                    String[] row = new String[n];
                    for (int i = 0; i < n; i++) {
                        row[i] = text(rs.getObject(i + 1));
                    }
                    table.add(row);
                }
                int[] w = new int[n];
                for (String[] row : table) {
                    for (int i = 0; i < n; i++) {
                        w[i] = Math.max(w[i], row[i].length());
                    }
                }
                for (int r = 0; r < table.size(); r++) {
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < n; i++) {
                        String cell = table.get(r)[i];
                        sb.append(i == n - 1 ? cell : String.format("%-" + w[i] + "s ", cell));
                    }
                    System.out.println(sb);
                    if (r == 0) {
                        StringBuilder dash = new StringBuilder();
                        for (int i = 0; i < n; i++) {
                            dash.append("-".repeat(w[i])).append(i == n - 1 ? "" : " ");
                        }
                        System.out.println(dash);
                    }
                }
            }
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("profiler", args);
            try (Connection con = attachOrCreate(path)) {
                // --- workload fixtures (auto-commit: each DDL commits) ----------
                try {
                    execute(con, "drop procedure hotspot");
                } catch (SQLException ignored) {
                    // first run
                }
                execute(con, "recreate table nums (id int primary key, val int)");
                execute(con, "execute block as declare n int = 0; begin "
                        + "  while (n < 5000) do begin "
                        + "    insert into nums values (:n, mod(:n, 97)); n = n + 1; end end");
                execute(con, HOTSPOT);

                // --- profile inside one SNAPSHOT transaction --------------------
                con.setAutoCommit(false);
                con.setTransactionIsolation(Connection.TRANSACTION_REPEATABLE_READ);
                long profileId = ((Number) scalar(con,
                        "select rdb$profiler.start_session('java hands-on') from rdb$database")).longValue();
                scalar(con, "select count(*) from nums a join nums b on b.id = a.val");
                scalar(con, "select total from hotspot");
                execute(con, "execute procedure rdb$profiler.finish_session(true)");
                System.out.println("profile session " + profileId + " finished and flushed");
                System.out.println("PSQL stat rows visible inside the SNAPSHOT that ran it: "
                        + scalar(con, "select count(*) from plg$profiler.plg$prof_psql_stats "
                                + "where profile_id = " + profileId)
                        + "  <- the flush committed autonomously, after the snapshot");
                con.commit();                       // a real commit, not commit-retaining

                System.out.println();
                System.out.println("record sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):");
                print(con, "select cast(lpad('', level * 2) || cast(access_path as varchar(120)) "
                        + "           as varchar(140)) as access_path, "
                        + "       open_counter as opens, fetch_counter as fetches, "
                        + "       open_fetch_total_elapsed_time as total_ns "
                        + "from plg$profiler.plg$prof_record_source_stats_view "
                        + "where profile_id = ? and sql_text containing 'join nums' "
                        + "order by cursor_id, record_source_id", profileId);

                System.out.println();
                System.out.println("hotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):");
                print(con, "select line_num, column_num, counter, "
                        + "       total_elapsed_time as total_ns, avg_elapsed_time as avg_ns "
                        + "from plg$profiler.plg$prof_psql_stats_view "
                        + "where profile_id = ? and routine_name = 'HOTSPOT' "
                        + "order by total_elapsed_time desc", profileId);
                con.commit();
            }
            System.out.println("done.");
        });
    }
}
