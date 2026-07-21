/*
 *  pooling.cpp — the external-connections (EDS) pool, watched live.
 *
 *  Companion to ../../connection-pooling.md.  The server acts as a CLIENT of
 *  another data source via EXECUTE STATEMENT ... ON EXTERNAL, and the engine
 *  pools those outbound connections.  This sample tunes the pool at runtime
 *  (ALTER EXTERNAL CONNECTIONS POOL, needs the MODIFY_EXT_CONN_POOL system
 *  privilege), then makes three external calls to the same data source and
 *  reads the pool's context variables at each stage:
 *
 *      before:            idle 0, active 0
 *      inside the block:  idle 0, active 1   <- three calls, ONE connection
 *      after commit:      idle 1, active 0   <- reset, back on the idle list
 *      after CLEAR ALL:   idle 0             <- evicted
 */

#include "fb_sample.h"

using namespace Firebird;

// The pool's four SYSTEM context variables in one row.
static void poolState(fbsample::Db& db, ITransaction* tra, const char* moment)
{
	fbsample::Db::Table t = db.query(tra,
		"select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_SIZE'),"
		"       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_LIFETIME'),"
		"       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT'),"
		"       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT')"
		" from rdb$database");
	printf("%-18s size=%s lifetime=%ss idle=%s active=%s\n", moment,
		t.rows[0][0].c_str(), t.rows[0][1].c_str(),
		t.rows[0][2].c_str(), t.rows[0][3].c_str());
}

int main(int argc, char** argv)
{
	const char* database = fbsample::argOrDefault(argc, argv, 1,
		"inet://localhost/employee");
	const char* external = fbsample::argOrDefault(argc, argv, 2,
		"inet://localhost/employee");    // the DSN the SERVER will connect to

	try
	{
		fbsample::Db db;
		db.attach(database);

		// 1. Tune the pool at runtime (per server process, not persistent).
		ITransaction* tra = db.start();
		db.exec(tra, "alter external connections pool set size 5");
		db.exec(tra, "alter external connections pool set lifetime 30 second");
		tra->commitRetaining(&db.status);

		poolState(db, tra, "before:");

		// 2. Three EXECUTE STATEMENT ON EXTERNAL calls to the SAME
		//    (connection string, user, password, role) — the pool's key.
		std::string block =
			"execute block returns (idle varchar(10), active varchar(10)) as\n"
			"  declare i int = 0;\n"
			"  declare v int;\n"
			"begin\n"
			"  while (i < 3) do\n"
			"  begin\n"
			"    execute statement 'select 1 from rdb$database'\n"
			"      on external '" + std::string(external) + "'\n"
			"      as user '" + fbsample::Db::defaultUser() + "'"
			" password '" + fbsample::Db::defaultPassword() + "'\n"
			"      into :v;\n"
			"    i = i + 1;\n"
			"  end\n"
			"  idle   = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');\n"
			"  active = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');\n"
			"  suspend;\n"
			"end";
		fbsample::Db::Table in = db.query(tra, block);
		printf("%-18s idle=%s active=%s   (3 calls, 1 outbound connection)\n",
			"inside the block:", in.rows[0][0].c_str(), in.rows[0][1].c_str());

		// 3. Full commit: only now is the external connection truly unused —
		//    it is reset with ALTER SESSION RESET and parked on the idle
		//    list.  (COMMIT RETAINING keeps the transaction context alive,
		//    and with it the pooled connection stays ACTIVE — try it.)
		tra->commit(&db.status);
		tra = db.start();
		poolState(db, tra, "after commit:");

		// 4. Evict every idle connection now.
		db.exec(tra, "alter external connections pool clear all");
		poolState(db, tra, "after CLEAR ALL:");

		tra->commit(&db.status);
		printf("done.\n");
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
	return 0;
}
