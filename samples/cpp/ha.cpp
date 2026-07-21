/*
 *  ha.cpp — companion sample for ../../high-availability.md
 *
 *  The one HA primitive that is pure client-side SQL: a database SHADOW.
 *  The sample creates a shadow on a scratch database, proves the mirror
 *  file appears (RDB$FILES + a stat() of the file — server and sample run
 *  on the same host here), shows the shadow growing in lock-step with the
 *  main file as rows are inserted, and drops it again.
 *
 *  The other primitives (replica promotion with gfix -replica none,
 *  sync_replica) need server-side config and stay as text in the document.
 */

#include "fb_sample.h"
#include <sys/stat.h>

using namespace Firebird;

static const char* DB   = "inet://localhost//tmp/fbhandson/ha.fdb";
static const char* MAIN = "/tmp/fbhandson/ha.fdb";      // server-side paths
static const char* SHAD = "/tmp/fbhandson/ha.shd";

static long fileSize(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 ? (long) st.st_size : -1;
}

static void showFiles(const char* when)
{
	printf("%-28s main = %8ld bytes, shadow = %8ld bytes\n",
		when, fileSize(MAIN), fileSize(SHAD));
}

int main(int argc, char** argv)
{
	try
	{
		fbsample::Db db;
		db.attachOrCreate(fbsample::argOrDefault(argc, argv, 1, DB));
		ITransaction* tra = db.start();

		// Idempotent cleanup from earlier runs.
		try { db.exec(tra, "DROP SHADOW 1 DELETE FILE"); } catch (const FbException&) {}
		try { db.exec(tra, "DROP TABLE HA_LOG"); } catch (const FbException&) {}
		db.exec(tra, "CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))");
		tra->commitRetaining(&db.status);

		// 1. Create the synchronous page-level mirror.
		db.exec(tra, "CREATE SHADOW 1 '/tmp/fbhandson/ha.shd'");
		tra->commitRetaining(&db.status);
		printf("CREATE SHADOW 1 done — the engine dumped every page to the mirror\n\n");

		// The shadow is registered in the metadata like any other file.
		fbsample::Db::print(db.query(tra,
			"SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS "
			"FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER"));
		printf("\n");
		showFiles("after CREATE SHADOW:");

		// 2. Write load: every page write now goes to both files.
		db.exec(tra,
			"EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
			"  WHILE (I < 5000) DO BEGIN "
			"    INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1; "
			"  END "
			"END");
		tra->commitRetaining(&db.status);
		showFiles("after 5000 inserts:");

		// 3. Retire the mirror.
		db.exec(tra, "DROP SHADOW 1 DELETE FILE");
		tra->commit(&db.status);
		printf("\nDROP SHADOW 1 DELETE FILE done\n");
		showFiles("after DROP SHADOW:");

		printf("\nRDB$FILES rows left: %s\n",
			[&db]() { ITransaction* t2 = db.start();
			          std::string n = db.queryValue(t2, "SELECT COUNT(*) FROM RDB$FILES");
			          t2->commit(&db.status); return n; }().c_str());
		printf("done.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
}
