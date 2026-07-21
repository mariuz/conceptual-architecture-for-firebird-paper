/*
 *  deployment.cpp (fb-cpp) — the engine's self-portrait, typed edition.
 *
 *  The fb-cpp twin of ../cpp/deployment.cpp: the same three layers —
 *  MON$DATABASE, RDB$CONFIG, the SYSTEM context namespace — read back from
 *  the shared employee database.  Where the OO-API sample coerces every
 *  output column to VARCHAR server-side, fb-cpp's getString() renders
 *  numbers and booleans client-side, so the query text stays plain
 *  SELECT and the conversion lives in the wrapper.  Read-only.
 *  See ../../deployment-and-operations.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_deployment [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DB = "inet://localhost/employee";

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DB);

	try
	{
		Client client{"fbclient"};
		Attachment att{client, database, defaultOptions()};
		Transaction tra{att};

		auto line = [&](const char* label, const char* sql)
		{
			Statement stmt{att, tra, sql};
			stmt.execute(tra);
			printf("  %-22s %s\n", label, stmt.getString(0).value_or("<null>").c_str());
		};

		printf("== MON$DATABASE: the database as deployed ==\n");
		line("database file",  "SELECT MON$DATABASE_NAME FROM MON$DATABASE");
		line("ODS version",    "SELECT MON$ODS_MAJOR || '.' || MON$ODS_MINOR FROM MON$DATABASE");
		line("page size",      "SELECT MON$PAGE_SIZE FROM MON$DATABASE");
		line("page buffers",   "SELECT MON$PAGE_BUFFERS FROM MON$DATABASE");
		line("sweep interval", "SELECT MON$SWEEP_INTERVAL FROM MON$DATABASE");
		line("forced writes",  "SELECT MON$FORCED_WRITES FROM MON$DATABASE");
		line("SQL dialect",    "SELECT MON$SQL_DIALECT FROM MON$DATABASE");
		line("crypt state",    "SELECT MON$CRYPT_STATE FROM MON$DATABASE");

		Statement count{att, tra, "SELECT COUNT(*) FROM RDB$CONFIG"};
		count.execute(tra);
		printf("\n== RDB$CONFIG: effective configuration (selected of %s settings) ==\n",
			count.getString(0).value_or("?").c_str());
		Statement cfg{att, tra,
			"SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_IS_SET "
			"FROM RDB$CONFIG "
			"WHERE RDB$CONFIG_NAME IN ('ServerMode', 'DefaultDbCachePages', "
			"  'DatabaseAccess', 'WireCrypt', 'MaxParallelWorkers', 'SecurityDatabase') "
			"ORDER BY RDB$CONFIG_NAME"};
		for (bool ok = cfg.execute(tra); ok; ok = cfg.fetchNext())
		{
			printf("  %-19s %-27s is_set=%s\n",
				cfg.getString(0).value_or("<null>").c_str(),
				cfg.getString(1).value_or("<null>").c_str(),
				cfg.getBool(2).value_or(false) ? "true" : "false");
		}

		printf("\n== settings explicitly set in config files ==\n");
		Statement set{att, tra,
			"SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_SOURCE "
			"FROM RDB$CONFIG WHERE RDB$CONFIG_IS_SET ORDER BY RDB$CONFIG_ID"};
		bool any = false;
		for (bool ok = set.execute(tra); ok; ok = set.fetchNext(), any = true)
		{
			printf("  %-19s %-27s %s\n",
				set.getString(0).value_or("<null>").c_str(),
				set.getString(1).value_or("<null>").c_str(),
				set.getString(2).value_or("<null>").c_str());
		}
		if (!any)
			printf("  (none — this server runs on stock defaults)\n");

		printf("\n== SYSTEM context: this engine, this session ==\n");
		line("ENGINE_VERSION",   "SELECT RDB$GET_CONTEXT('SYSTEM','ENGINE_VERSION') FROM RDB$DATABASE");
		line("DB_NAME",          "SELECT RDB$GET_CONTEXT('SYSTEM','DB_NAME') FROM RDB$DATABASE");
		line("NETWORK_PROTOCOL", "SELECT RDB$GET_CONTEXT('SYSTEM','NETWORK_PROTOCOL') FROM RDB$DATABASE");
		line("WIRE_CRYPT_PLUGIN","SELECT RDB$GET_CONTEXT('SYSTEM','WIRE_CRYPT_PLUGIN') FROM RDB$DATABASE");
		line("CLIENT_ADDRESS",   "SELECT RDB$GET_CONTEXT('SYSTEM','CLIENT_ADDRESS') FROM RDB$DATABASE");

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
