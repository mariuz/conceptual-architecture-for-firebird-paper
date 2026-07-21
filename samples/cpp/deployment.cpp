/*
 *  deployment.cpp — companion sample for ../../deployment-and-operations.md
 *
 *  A deployment is usually inspected from the server's shell (config files,
 *  systemd, /opt/firebird) — but the engine also publishes its own view of
 *  it to any SQL client, and that is what this sample reads, three layers
 *  deep:
 *
 *    1. MON$DATABASE            — the physical facts of this database
 *    2. RDB$CONFIG              — the EFFECTIVE configuration (firebird.conf
 *                                 merged with databases.conf, per database)
 *    3. SYSTEM context vars     — engine version, protocol, session facts
 *
 *  Read-only: safe to run against the shared employee database (default).
 */

#include "fb_sample.h"

using namespace Firebird;

static const char* DB = "inet://localhost/employee";

int main(int argc, char** argv)
{
	try
	{
		fbsample::Db db;
		db.attach(fbsample::argOrDefault(argc, argv, 1, DB));
		ITransaction* tra = db.start();

		auto line = [&](const char* label, const std::string& sql)
		{ printf("  %-22s %s\n", label, db.queryValue(tra, sql).c_str()); };

		printf("== MON$DATABASE: the database as deployed ==\n");
		line("database file",  "SELECT MON$DATABASE_NAME FROM MON$DATABASE");
		line("ODS version",    "SELECT MON$ODS_MAJOR || '.' || MON$ODS_MINOR FROM MON$DATABASE");
		line("page size",      "SELECT MON$PAGE_SIZE FROM MON$DATABASE");
		line("page buffers",   "SELECT MON$PAGE_BUFFERS FROM MON$DATABASE");
		line("sweep interval", "SELECT MON$SWEEP_INTERVAL FROM MON$DATABASE");
		line("forced writes",  "SELECT MON$FORCED_WRITES FROM MON$DATABASE");
		line("SQL dialect",    "SELECT MON$SQL_DIALECT FROM MON$DATABASE");
		line("crypt state",    "SELECT MON$CRYPT_STATE FROM MON$DATABASE");

		printf("\n== RDB$CONFIG: effective configuration (selected of %s settings) ==\n",
			db.queryValue(tra, "SELECT COUNT(*) FROM RDB$CONFIG").c_str());
		fbsample::Db::print(db.query(tra,
			"SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_IS_SET "
			"FROM RDB$CONFIG "
			"WHERE RDB$CONFIG_NAME IN ('ServerMode', 'DefaultDbCachePages', "
			"  'DatabaseAccess', 'WireCrypt', 'MaxParallelWorkers', 'SecurityDatabase') "
			"ORDER BY RDB$CONFIG_NAME"));

		printf("\n== settings explicitly set in config files ==\n");
		fbsample::Db::print(db.query(tra,
			"SELECT RDB$CONFIG_NAME, RDB$CONFIG_VALUE, RDB$CONFIG_SOURCE "
			"FROM RDB$CONFIG WHERE RDB$CONFIG_IS_SET ORDER BY RDB$CONFIG_ID"));

		printf("\n== SYSTEM context: this engine, this session ==\n");
		line("ENGINE_VERSION",   "SELECT RDB$GET_CONTEXT('SYSTEM','ENGINE_VERSION') FROM RDB$DATABASE");
		line("DB_NAME",          "SELECT RDB$GET_CONTEXT('SYSTEM','DB_NAME') FROM RDB$DATABASE");
		line("NETWORK_PROTOCOL", "SELECT RDB$GET_CONTEXT('SYSTEM','NETWORK_PROTOCOL') FROM RDB$DATABASE");
		line("WIRE_CRYPT_PLUGIN","SELECT RDB$GET_CONTEXT('SYSTEM','WIRE_CRYPT_PLUGIN') FROM RDB$DATABASE");
		line("CLIENT_ADDRESS",   "SELECT RDB$GET_CONTEXT('SYSTEM','CLIENT_ADDRESS') FROM RDB$DATABASE");

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
}
