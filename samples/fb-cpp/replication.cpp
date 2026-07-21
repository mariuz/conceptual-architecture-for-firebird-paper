/*
 *  replication.cpp (fb-cpp) — the publication state walk.
 *
 *  The fb-cpp twin of ../cpp/replication.cpp: the same client-visible half
 *  of replication — ALTER DATABASE ENABLE PUBLICATION / INCLUDE TABLE /
 *  INCLUDE ALL, with RDB$PUBLICATIONS and RDB$PUBLICATION_TABLES read back
 *  after every step.  Plain DDL plus system tables; scratch-database
 *  publication state only, no replication.conf, no restart.
 *  See ../../replication-architecture.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_replication [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DB = "inet://localhost//tmp/fbhandson/replication_fbcpp.fdb";

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DB);

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		Transaction tra{att};

		auto exec = [&](const char* sql) { Statement{att, tra, sql}.execute(tra); };

		auto pubState = [&](const char* when)
		{
			Statement pub{att, tra,
				"SELECT RDB$PUBLICATION_NAME, RDB$ACTIVE_FLAG, RDB$AUTO_ENABLE "
				"FROM RDB$PUBLICATIONS"};
			pub.execute(tra);

			std::string tables;
			Statement members{att, tra,
				"SELECT RDB$TABLE_SCHEMA_NAME, RDB$TABLE_NAME "
				"FROM RDB$PUBLICATION_TABLES ORDER BY RDB$TABLE_NAME"};
			for (bool ok = members.execute(tra); ok; ok = members.fetchNext())
			{
				if (!tables.empty())
					tables += ", ";
				tables += members.getString(0).value_or("?") + "."
					+ members.getString(1).value_or("?");
			}

			printf("-- %s\n", when);
			printf("%s   ACTIVE_FLAG %s   AUTO_ENABLE %s    published: %s\n",
				pub.getString(0).value_or("<null>").c_str(),
				pub.getString(1).value_or("<null>").c_str(),
				pub.getString(2).value_or("<null>").c_str(),
				tables.empty() ? "(none)" : tables.c_str());
		};

		// Idempotent reset: back to a clean, unpublished state.
		try { exec("ALTER DATABASE EXCLUDE ALL FROM PUBLICATION"); } catch (const DatabaseException&) {}
		try { exec("ALTER DATABASE DISABLE PUBLICATION"); } catch (const DatabaseException&) {}
		try { exec("DROP TABLE REPL_ORDERS"); } catch (const DatabaseException&) {}
		try { exec("DROP TABLE REPL_SCRATCH"); } catch (const DatabaseException&) {}
		exec("CREATE TABLE REPL_ORDERS (ID INT NOT NULL PRIMARY KEY, ITEM VARCHAR(30))");
		exec("CREATE TABLE REPL_SCRATCH (N INT)");   // note: no key
		tra.commitRetaining();

		pubState("initial state (publication exists but is inactive)");

		exec("ALTER DATABASE ENABLE PUBLICATION");
		tra.commitRetaining();
		pubState("after ENABLE PUBLICATION");

		exec("ALTER DATABASE INCLUDE TABLE REPL_ORDERS TO PUBLICATION");
		tra.commitRetaining();
		pubState("after INCLUDE TABLE REPL_ORDERS");

		exec("ALTER DATABASE INCLUDE ALL TO PUBLICATION");
		tra.commitRetaining();
		pubState("after INCLUDE ALL (auto-enable: future tables join automatically)");

		// The monitoring view of the same facts, on this attachment.
		Statement mode{att, tra, "SELECT MON$REPLICA_MODE FROM MON$DATABASE"};
		mode.execute(tra);
		printf("\nMON$REPLICA_MODE = %s\n", mode.getString(0).value_or("<null>").c_str());

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
