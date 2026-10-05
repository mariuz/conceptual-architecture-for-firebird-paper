//
// Schemas.java - SQL schemas and the search path (Java twin of
// ../../../../../cpp/schemas.cpp; see
// ../../../../../../schemas-and-name-resolution.md).
//
// The same five demonstrations: RDB$SCHEMAS and the default path, one
// unqualified SELECT changing meaning as SET SEARCH_PATH changes, the
// SYSTEM auto-append, a procedure that keeps binding its own schema after
// the session's path flips, and the plan naming the resolved table
// (FirebirdPreparedStatement.getExecutionPlan(), plus the explained form).
// The instructive difference is JDBC's own schema surface: Jaybird 6
// predates Firebird 6 schemas, so DatabaseMetaData.getSchemas() is empty,
// supportsSchemasInDataManipulation() is false and getTables() reports
// both CUSTOMERS tables with TABLE_SCHEM null - the engine resolves names
// by schema while the JDBC metadata layer cannot tell the two apart.
// (Schema support in the metadata API is Jaybird 7 work.)
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Schemas
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.rows;
import static fbsamples.FbSample.scalar;

import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.StringJoiner;

import org.firebirdsql.jdbc.FirebirdPreparedStatement;

public final class Schemas {

    private static Object path(Connection con) throws SQLException {
        return scalar(con, "select rdb$get_context('SYSTEM', 'SEARCH_PATH') from rdb$database");
    }

    private static Object origin(Connection con) throws SQLException {
        return scalar(con, "select origin from customers");
    }

    private static void quietly(Connection con, String sql) {
        try {
            execute(con, sql);
        } catch (SQLException ignored) {
            // did not exist
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String db = dbPath("schemas", args);
            try (Connection con = attachOrCreate(db)) {
                // Idempotent cleanup + setup (auto-commit: one transaction each).
                for (String s : new String[] {"drop procedure app.which_one",
                        "drop table public.customers", "drop table app.customers", "drop schema app"}) {
                    quietly(con, s);
                }
                execute(con, "create schema app");
                execute(con, "create table public.customers (id int, origin varchar(20))");
                execute(con, "create table app.customers    (id int, origin varchar(20))");
                execute(con, "insert into public.customers values (1, 'from PUBLIC')");
                execute(con, "insert into app.customers    values (2, 'from APP')");

                // 1. The catalog and the default path.
                StringJoiner names = new StringJoiner("  ");
                for (Object[] r : rows(con, "select trim(rdb$schema_name) from rdb$schemas order by 1")) {
                    names.add(r[0].toString());
                }
                System.out.println("schemas in RDB$SCHEMAS      : " + names);
                System.out.println("default search path         : " + path(con));

                // 2. Same statement, two resolutions.
                System.out.println();
                System.out.println("SELECT ORIGIN FROM CUSTOMERS, as the path changes:");
                System.out.println("  path PUBLIC,SYSTEM        -> " + origin(con));
                execute(con, "set search_path to app, public");
                System.out.println("  path APP,PUBLIC           -> " + origin(con));

                // 3. SYSTEM can be moved but not removed.
                execute(con, "set search_path to app");
                System.out.println();
                System.out.println("SET SEARCH_PATH TO APP      -> " + path(con) + "   (SYSTEM auto-appended)");

                // 4. Stored code binds its own schema, not the caller's path.
                execute(con, "set search_path to app, public");
                execute(con, "create procedure which_one returns (src varchar(20)) as "
                        + "begin select origin from customers into :src; suspend; end");
                System.out.println();
                System.out.println("procedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)");
                execute(con, "set search_path to public");
                System.out.println("  after SET SEARCH_PATH TO PUBLIC:");
                System.out.println("    direct SELECT ... FROM CUSTOMERS -> " + origin(con));
                System.out.println("    SELECT SRC FROM APP.WHICH_ONE    -> "
                        + scalar(con, "select src from app.which_one") + "   <- unmoved");
                System.out.println("    RDB$DEPENDENCIES records         -> " + scalar(con,
                        "select trim(rdb$depended_on_schema_name) || '.' || trim(rdb$depended_on_name)"
                                + " from rdb$dependencies where rdb$dependent_name = 'WHICH_ONE'"));

                // 5. Plans are schema-qualified.
                try (PreparedStatement ps = con.prepareStatement("select count(*) from customers")) {
                    FirebirdPreparedStatement fps = ps.unwrap(FirebirdPreparedStatement.class);
                    System.out.println();
                    System.out.println("plan for unqualified SELECT : " + fps.getExecutionPlan().strip());
                    System.out.println("explained plan              : "
                            + fps.getExplainedExecutionPlan().strip().replace("\n", "\n                              "));
                }

                // 6. ...and what JDBC's own metadata layer makes of it.
                DatabaseMetaData md = con.getMetaData();
                int schemas = 0;
                try (ResultSet rs = md.getSchemas()) {
                    while (rs.next()) {
                        schemas++;
                    }
                }
                System.out.println();
                System.out.println("JDBC DatabaseMetaData (" + md.getDriverName() + " " + md.getDriverVersion() + "):");
                System.out.println("  supportsSchemasInDataManipulation() -> "
                        + md.supportsSchemasInDataManipulation());
                System.out.println("  getSchemas()                        -> " + schemas + " rows");
                System.out.println("  Connection.getSchema()              -> " + con.getSchema());
                try (ResultSet rs = md.getTables(null, null, "CUSTOMERS", null)) {
                    while (rs.next()) {
                        System.out.println("  getTables(\"CUSTOMERS\")              -> TABLE_SCHEM="
                                + rs.getString("TABLE_SCHEM") + " TABLE_NAME=" + rs.getString("TABLE_NAME"));
                    }
                }
            }
            System.out.println();
            System.out.println("done.");
        });
    }

    private Schemas() {
    }
}
