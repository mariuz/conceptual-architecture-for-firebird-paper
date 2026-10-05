//
// Indexes.java - one B-tree, many variants (Java twin of
// ../../../../../cpp/indexes.cpp; see
// ../../../../../../indexing-and-full-text-search.md).
//
// A 3,000-row table gets a descending, an expression (COMPUTED BY), a
// partial (WHERE) and a plain index; five queries are prepared and their
// plans printed: expression index, partial index, descending navigation,
// a two-index bitmap OR, and CONTAINING falling to NATURAL.  Jaybird asks
// the *statement*, as the C++ sample does: FirebirdPreparedStatement
// returns the legacy one-line PLAN from getExecutionPlan() and the
// explained tree from getExplainedExecutionPlan() (isc_info_sql_get_plan /
// isc_info_sql_explain_plan in the prepare-info request).  The JDBC
// addition is DatabaseMetaData.getIndexInfo, the portable index catalog,
// which shows what a generic tool can and cannot see of the variants.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Indexes
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;

import org.firebirdsql.jdbc.FirebirdPreparedStatement;

public final class Indexes {

    private static void plan(Connection con, String sql, boolean explained) throws SQLException {
        try (PreparedStatement ps = con.prepareStatement(sql)) {
            FirebirdPreparedStatement fps = ps.unwrap(FirebirdPreparedStatement.class);
            System.out.println(sql);
            System.out.println(fps.getExecutionPlan().strip());
            if (explained) {
                System.out.println("explained:");
                System.out.println("  " + fps.getExplainedExecutionPlan().strip().replace("\n", "\n  "));
            }
            System.out.println();
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            try (Connection con = attachOrCreate(dbPath("indexes", args))) {
                execute(con, "recreate table doc ("
                        + " id integer, title varchar(60), status varchar(10), num integer)");
                execute(con, "execute block as declare i integer = 0; begin"
                        + "  while (i < 3000) do begin"
                        + "    insert into doc values (:i, 'Title ' || :i,"
                        + "      iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100));"
                        + "    i = i + 1;"
                        + "  end "
                        + "end");
                execute(con, "create descending index doc_id_desc on doc (id)");
                execute(con, "create index doc_upper_title on doc computed by (upper(title))");
                execute(con, "create index doc_active on doc (status) where status = 'active'");
                execute(con, "create index doc_num on doc (num)");
                System.out.println("3000 rows; indexes: descending, expression, partial, plain\n");

                plan(con, "select id from doc where upper(title) = 'TITLE 5'", false);
                plan(con, "select id from doc where status = 'active'", false);
                plan(con, "select first 1 id from doc order by id desc", false);
                plan(con, "select id from doc where num = 42 or id = 7", true);
                plan(con, "select id from doc where title containing 'itle 12'", false);

                System.out.println("CONTAINING is correct but unindexed: matched "
                        + scalar(con, "select count(*) from doc where title containing 'itle 12'")
                        + " rows by scanning all 3000\n");

                // The portable view: what JDBC's index catalog reports.
                System.out.println("DatabaseMetaData.getIndexInfo(DOC):");
                try (ResultSet rs = con.getMetaData().getIndexInfo(null, null, "DOC", false, false)) {
                    while (rs.next()) {
                        System.out.printf("  %-16s column=%-14s asc/desc=%s filter=%s%n",
                                rs.getString("INDEX_NAME"), rs.getString("COLUMN_NAME"),
                                rs.getString("ASC_OR_DESC"), rs.getString("FILTER_CONDITION"));
                    }
                }
            }
            System.out.println("done.");
        });
    }
}
