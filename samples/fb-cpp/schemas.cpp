/*
 *  schemas.cpp (fb-cpp) — name resolution watched through typed statements.
 *
 *  The fb-cpp twin of ../cpp/schemas.cpp: the same five demonstrations —
 *  RDB$SCHEMAS and the default path, one unqualified SELECT resolving
 *  differently as SET SEARCH_PATH changes, SYSTEM auto-appended, stored
 *  code binding its OWN schema forever, and the schema-qualified plan.
 *  Nothing schema-specific needs the API: search path is session state the
 *  engine keeps, so the twin is a straight port — the plan simply comes
 *  from StatementOptions().setPrefetchLegacyPlan() instead of a getPlan()
 *  call on IStatement.  See ../../schemas-and-name-resolution.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_schemas [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static void exec(Attachment& att, Transaction& tra, const char* sql)
{
	Statement{att, tra, sql}.execute(tra);
}

static std::string one(Attachment& att, Transaction& tra, const char* sql)
{
	Statement stmt{att, tra, sql};
	stmt.execute(tra);
	return stmt.getString(0).value_or("<null>");
}

static std::string ctxPath(Attachment& att, Transaction& tra)
{
	return one(att, tra,
		"SELECT RDB$GET_CONTEXT('SYSTEM','SEARCH_PATH') FROM RDB$DATABASE");
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/schemas_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		// -- Idempotent cleanup + setup.
		{
			Transaction ddl{att};
			for (const char* s : { "DROP PROCEDURE APP.WHICH_ONE",
					"DROP TABLE PUBLIC.CUSTOMERS", "DROP TABLE APP.CUSTOMERS",
					"DROP SCHEMA APP" })
				try { exec(att, ddl, s); } catch (const DatabaseException&) {}
			exec(att, ddl, "CREATE SCHEMA APP");
			exec(att, ddl, "CREATE TABLE PUBLIC.CUSTOMERS (ID INT, ORIGIN VARCHAR(20))");
			exec(att, ddl, "CREATE TABLE APP.CUSTOMERS    (ID INT, ORIGIN VARCHAR(20))");
			ddl.commit();
		}

		// -- 1. The catalog and the default path.
		Transaction tra{att};
		exec(att, tra, "INSERT INTO PUBLIC.CUSTOMERS VALUES (1, 'from PUBLIC')");
		exec(att, tra, "INSERT INTO APP.CUSTOMERS    VALUES (2, 'from APP')");
		printf("schemas in RDB$SCHEMAS      : ");
		{
			Statement s{att, tra,
				"SELECT TRIM(RDB$SCHEMA_NAME) FROM RDB$SCHEMAS ORDER BY 1"};
			for (bool ok = s.execute(tra); ok; ok = s.fetchNext())
				printf("%s  ", s.getString(0)->c_str());
		}
		printf("\ndefault search path         : %s\n", ctxPath(att, tra).c_str());

		// -- 2. Same statement, two resolutions.
		printf("\nSELECT ORIGIN FROM CUSTOMERS, as the path changes:\n");
		printf("  path PUBLIC,SYSTEM        -> %s\n",
			one(att, tra, "SELECT ORIGIN FROM CUSTOMERS").c_str());
		exec(att, tra, "SET SEARCH_PATH TO APP, PUBLIC");
		printf("  path APP,PUBLIC           -> %s\n",
			one(att, tra, "SELECT ORIGIN FROM CUSTOMERS").c_str());

		// -- 3. SYSTEM can be moved but not removed.
		exec(att, tra, "SET SEARCH_PATH TO APP");
		printf("\nSET SEARCH_PATH TO APP      -> %s   (SYSTEM auto-appended)\n",
			ctxPath(att, tra).c_str());

		// -- 4. Stored code binds its own schema, not the caller's path.
		exec(att, tra, "SET SEARCH_PATH TO APP, PUBLIC");
		exec(att, tra,
			"CREATE PROCEDURE WHICH_ONE RETURNS (SRC VARCHAR(20)) AS "
			"BEGIN SELECT ORIGIN FROM CUSTOMERS INTO :SRC; SUSPEND; END");
		tra.commit();

		Transaction tra2{att};
		printf("\nprocedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)\n");
		exec(att, tra2, "SET SEARCH_PATH TO PUBLIC");
		printf("  after SET SEARCH_PATH TO PUBLIC:\n");
		printf("    direct SELECT ... FROM CUSTOMERS -> %s\n",
			one(att, tra2, "SELECT ORIGIN FROM CUSTOMERS").c_str());
		printf("    SELECT SRC FROM APP.WHICH_ONE    -> %s   <- unmoved\n",
			one(att, tra2, "SELECT SRC FROM APP.WHICH_ONE").c_str());
		printf("    RDB$DEPENDENCIES records         -> %s\n",
			one(att, tra2,
				"SELECT TRIM(RDB$DEPENDED_ON_SCHEMA_NAME) || '.' || TRIM(RDB$DEPENDED_ON_NAME)"
				" FROM RDB$DEPENDENCIES WHERE RDB$DEPENDENT_NAME = 'WHICH_ONE'").c_str());

		// -- 5. Plans are schema-qualified.
		{
			Statement s{att, tra2, "SELECT COUNT(*) FROM CUSTOMERS",
				StatementOptions().setPrefetchLegacyPlan(true)};
			printf("\nplan for unqualified SELECT :%s\n", s.getLegacyPlan().c_str());
		}

		tra2.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
