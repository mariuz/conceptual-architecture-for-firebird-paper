/*
 *  schemas.cpp — companion sample for schemas-and-name-resolution.md.
 *
 *  Replays the document's core demonstrations programmatically:
 *
 *   1. RDB$SCHEMAS and the default search path ("PUBLIC", "SYSTEM").
 *   2. Two same-named tables (PUBLIC.CUSTOMERS / APP.CUSTOMERS); the same
 *      unqualified SELECT resolves differently as SET SEARCH_PATH changes.
 *   3. SYSTEM is auto-appended when omitted from SET SEARCH_PATH.
 *   4. A stored procedure created while APP leads the path binds
 *      APP.CUSTOMERS — and keeps meaning that after the session's path
 *      flips to PUBLIC (stored code resolves in its OWN schema, never the
 *      caller's path).  RDB$DEPENDENCIES records the resolution.
 *   5. getPlan() shows the schema-qualified plan.
 *
 *  Build/run: see ../../schemas-and-name-resolution.md (Hands-on section).
 */

#include "fb_sample.h"

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/schemas.fdb");
	try
	{
		Db db;
		db.attachOrCreate(database);

		auto ctxPath = [&](ITransaction* t) {
			return db.queryValue(t,
				"SELECT RDB$GET_CONTEXT('SYSTEM','SEARCH_PATH') FROM RDB$DATABASE");
		};

		// -- Idempotent cleanup + setup.
		ITransaction* ddl = db.start();
		for (const char* s : { "DROP PROCEDURE APP.WHICH_ONE",
				"DROP TABLE PUBLIC.CUSTOMERS", "DROP TABLE APP.CUSTOMERS",
				"DROP SCHEMA APP" })
			try { db.exec(ddl, s); } catch (const FbException&) {}
		db.exec(ddl, "CREATE SCHEMA APP");
		db.exec(ddl, "CREATE TABLE PUBLIC.CUSTOMERS (ID INT, ORIGIN VARCHAR(20))");
		db.exec(ddl, "CREATE TABLE APP.CUSTOMERS    (ID INT, ORIGIN VARCHAR(20))");
		ddl->commit(&db.status);

		// -- 1. The catalog and the default path.
		ITransaction* tra = db.start();
		db.exec(tra, "INSERT INTO PUBLIC.CUSTOMERS VALUES (1, 'from PUBLIC')");
		db.exec(tra, "INSERT INTO APP.CUSTOMERS    VALUES (2, 'from APP')");
		printf("schemas in RDB$SCHEMAS      : ");
		for (auto& r : db.query(tra,
				"SELECT TRIM(RDB$SCHEMA_NAME) FROM RDB$SCHEMAS ORDER BY 1").rows)
			printf("%s  ", r[0].c_str());
		printf("\ndefault search path         : %s\n", ctxPath(tra).c_str());

		// -- 2. Same statement, three resolutions.
		printf("\nSELECT ORIGIN FROM CUSTOMERS, as the path changes:\n");
		printf("  path PUBLIC,SYSTEM        -> %s\n",
			db.queryValue(tra, "SELECT ORIGIN FROM CUSTOMERS").c_str());
		db.exec(tra, "SET SEARCH_PATH TO APP, PUBLIC");
		printf("  path APP,PUBLIC           -> %s\n",
			db.queryValue(tra, "SELECT ORIGIN FROM CUSTOMERS").c_str());

		// -- 3. SYSTEM can be moved but not removed.
		db.exec(tra, "SET SEARCH_PATH TO APP");
		printf("\nSET SEARCH_PATH TO APP      -> %s   (SYSTEM auto-appended)\n",
			ctxPath(tra).c_str());

		// -- 4. Stored code binds its own schema, not the caller's path.
		db.exec(tra, "SET SEARCH_PATH TO APP, PUBLIC");
		db.exec(tra,
			"CREATE PROCEDURE WHICH_ONE RETURNS (SRC VARCHAR(20)) AS "
			"BEGIN SELECT ORIGIN FROM CUSTOMERS INTO :SRC; SUSPEND; END");
		tra->commit(&db.status);

		tra = db.start();
		printf("\nprocedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)\n");
		db.exec(tra, "SET SEARCH_PATH TO PUBLIC");
		printf("  after SET SEARCH_PATH TO PUBLIC:\n");
		printf("    direct SELECT ... FROM CUSTOMERS -> %s\n",
			db.queryValue(tra, "SELECT ORIGIN FROM CUSTOMERS").c_str());
		printf("    SELECT SRC FROM APP.WHICH_ONE    -> %s   <- unmoved\n",
			db.queryValue(tra, "SELECT SRC FROM APP.WHICH_ONE").c_str());
		printf("    RDB$DEPENDENCIES records         -> %s\n",
			db.queryValue(tra,
				"SELECT TRIM(RDB$DEPENDED_ON_SCHEMA_NAME) || '.' || TRIM(RDB$DEPENDED_ON_NAME)"
				" FROM RDB$DEPENDENCIES WHERE RDB$DEPENDENT_NAME = 'WHICH_ONE'").c_str());

		// -- 5. Plans are schema-qualified.
		IStatement* stmt = db.att->prepare(&db.status, tra, 0,
			"SELECT COUNT(*) FROM CUSTOMERS", SQL_DIALECT_V6, 0);
		printf("\nplan for unqualified SELECT :%s\n", stmt->getPlan(&db.status, false));
		stmt->free(&db.status);

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
