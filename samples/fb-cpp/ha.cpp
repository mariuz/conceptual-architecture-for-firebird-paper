/*
 *  ha.cpp (fb-cpp) — the shadow lifecycle, RAII edition.
 *
 *  The fb-cpp twin of ../cpp/ha.cpp: CREATE SHADOW / DROP SHADOW are plain
 *  DSQL, so the wrapper changes nothing about the mechanism — what changes
 *  is the client code around it: Statement one-liners instead of exec()
 *  helpers, std::optional fetches, and a fetch loop for RDB$FILES instead
 *  of the coerce-to-VARCHAR machinery.  See ../../high-availability.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_ha [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <sys/stat.h>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DB   = "inet://localhost//tmp/fbhandson/ha_fbcpp.fdb";
static const char* MAIN = "/tmp/fbhandson/ha_fbcpp.fdb";     // server-side paths
static const char* SHAD = "/tmp/fbhandson/ha_fbcpp.shd";

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
	const char* database = argOrDefault(argc, argv, 1, DB);

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		Transaction tra{att};

		auto exec = [&](const char* sql) { Statement{att, tra, sql}.execute(tra); };

		// Idempotent cleanup from earlier runs.
		try { exec("DROP SHADOW 1 DELETE FILE"); } catch (const DatabaseException&) {}
		try { exec("DROP TABLE HA_LOG"); } catch (const DatabaseException&) {}
		exec("CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))");
		tra.commitRetaining();

		// 1. Create the synchronous page-level mirror.
		exec("CREATE SHADOW 1 '/tmp/fbhandson/ha_fbcpp.shd'");
		tra.commitRetaining();
		printf("CREATE SHADOW 1 done — the engine dumped every page to the mirror\n\n");

		// The shadow is registered in the metadata like any other file.
		Statement files{att, tra,
			"SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS "
			"FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER"};
		for (bool ok = files.execute(tra); ok; ok = files.fetchNext())
		{
			printf("shadow file %s   number %s   flags %s\n",
				files.getString(0).value_or("<null>").c_str(),
				files.getString(1).value_or("<null>").c_str(),
				files.getString(2).value_or("<null>").c_str());
		}
		printf("\n");
		showFiles("after CREATE SHADOW:");

		// 2. Write load: every page write now goes to both files.
		exec("EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
			"  WHILE (I < 5000) DO BEGIN "
			"    INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1; "
			"  END "
			"END");
		tra.commitRetaining();
		showFiles("after 5000 inserts:");

		// 3. Retire the mirror.
		exec("DROP SHADOW 1 DELETE FILE");
		tra.commit();
		printf("\nDROP SHADOW 1 DELETE FILE done\n");
		showFiles("after DROP SHADOW:");

		Transaction tra2{att};
		Statement left{att, tra2, "SELECT COUNT(*) FROM RDB$FILES"};
		left.execute(tra2);
		printf("\nRDB$FILES rows left: %s\n", left.getString(0).value_or("?").c_str());
		tra2.commit();

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
