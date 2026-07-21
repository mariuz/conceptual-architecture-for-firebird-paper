/*
 *  replication.cpp — companion sample for ../../replication-architecture.md
 *
 *  The client-visible half of Firebird replication: the publication.  All of
 *  it is plain DDL plus system tables — no replication.conf, no restart:
 *
 *    ALTER DATABASE ENABLE PUBLICATION            -> RDB$PUBLICATIONS
 *    ALTER DATABASE INCLUDE TABLE ... / INCLUDE ALL -> RDB$PUBLICATION_TABLES
 *
 *  The journal/segment transport behind it (Publisher -> ChangeLog ->
 *  Applier) needs server-side replication.conf and stays as text in the
 *  document's validated walk-through.
 */

#include "fb_sample.h"

using namespace Firebird;

static const char* DB = "inet://localhost//tmp/fbhandson/replication.fdb";

int main(int argc, char** argv)
{
	try
	{
		fbsample::Db db;
		db.attachOrCreate(fbsample::argOrDefault(argc, argv, 1, DB));
		ITransaction* tra = db.start();

		auto pubState = [&](const char* when)
		{
			printf("-- %s\n", when);
			fbsample::Db::print(db.query(tra,
				"SELECT RDB$PUBLICATION_NAME, RDB$ACTIVE_FLAG, RDB$AUTO_ENABLE "
				"FROM RDB$PUBLICATIONS"));
			fbsample::Db::print(db.query(tra,
				"SELECT RDB$TABLE_SCHEMA_NAME, RDB$TABLE_NAME "
				"FROM RDB$PUBLICATION_TABLES ORDER BY RDB$TABLE_NAME"));
			printf("\n");
		};

		// Idempotent reset: back to a clean, unpublished state.
		try { db.exec(tra, "ALTER DATABASE EXCLUDE ALL FROM PUBLICATION"); } catch (const FbException&) {}
		try { db.exec(tra, "ALTER DATABASE DISABLE PUBLICATION"); } catch (const FbException&) {}
		try { db.exec(tra, "DROP TABLE REPL_ORDERS"); } catch (const FbException&) {}
		try { db.exec(tra, "DROP TABLE REPL_SCRATCH"); } catch (const FbException&) {}
		db.exec(tra, "CREATE TABLE REPL_ORDERS (ID INT NOT NULL PRIMARY KEY, ITEM VARCHAR(30))");
		db.exec(tra, "CREATE TABLE REPL_SCRATCH (N INT)");   // note: no key
		tra->commitRetaining(&db.status);

		pubState("initial state (publication exists but is inactive)");

		db.exec(tra, "ALTER DATABASE ENABLE PUBLICATION");
		tra->commitRetaining(&db.status);
		pubState("after ENABLE PUBLICATION");

		db.exec(tra, "ALTER DATABASE INCLUDE TABLE REPL_ORDERS TO PUBLICATION");
		tra->commitRetaining(&db.status);
		pubState("after INCLUDE TABLE REPL_ORDERS");

		db.exec(tra, "ALTER DATABASE INCLUDE ALL TO PUBLICATION");
		tra->commitRetaining(&db.status);
		pubState("after INCLUDE ALL (auto-enable: future tables join automatically)");

		// The monitoring view of the same facts, on this attachment.
		fbsample::Db::print(db.query(tra,
			"SELECT MON$REPLICA_MODE FROM MON$DATABASE"));

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
}
