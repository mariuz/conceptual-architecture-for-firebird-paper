/*
 *  backup.cpp (fb-cpp) — the gbak round trip through BackupManager.
 *
 *  The fb-cpp twin of ../cpp/backup.cpp: the same service-driven backup +
 *  restore, but the whole Services choreography — SPB_ATTACH, SPB_START,
 *  isc_action_svc_backup/restore, the isc_info_svc_line drain loop — is
 *  fb-cpp's BackupManager, purpose-built for this task.  What was forty
 *  lines of XpbBuilder and query() becomes two option objects and a
 *  verbose-output callback.  See ../../backup-and-recovery.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_backup [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string_view>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DB_SRC   = "inet://localhost//tmp/fbhandson/backup_fbcpp.fdb";
static const char* SRC_PATH = "/tmp/fbhandson/backup_fbcpp.fdb";            // server-side paths
static const char* FBK      = "/tmp/fbhandson/backup_fbcpp.fbk";
static const char* RES_PATH = "/tmp/fbhandson/backup_fbcpp_restored.fdb";
static const char* DB_REST  = "inet://localhost//tmp/fbhandson/backup_fbcpp_restored.fdb";

static void gbakLine(std::string_view line)
{
	printf("  gbak> %.*s\n", static_cast<int>(line.size()), line.data());
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DB_SRC);

	try
	{
		Client client{"fbclient"};

		// -- 1. scratch source database, kept attached during the backup ----
		Attachment att = attachOrCreate(client, database);
		{
			Transaction tra{att};
			try { Statement{att, tra, "DROP TABLE BR_ITEMS"}.execute(tra); }
			catch (const DatabaseException&) {}
			Statement{att, tra,
				"CREATE TABLE BR_ITEMS (ID INT NOT NULL PRIMARY KEY, NAME VARCHAR(30))"}
				.execute(tra);
			tra.commitRetaining();
			Statement{att, tra, "INSERT INTO BR_ITEMS VALUES (1, 'alpha')"}.execute(tra);
			Statement{att, tra, "INSERT INTO BR_ITEMS VALUES (2, 'beta')"}.execute(tra);
			Statement{att, tra, "INSERT INTO BR_ITEMS VALUES (3, 'gamma')"}.execute(tra);
			tra.commit();
		}
		printf("source ready: BR_ITEMS with 3 rows\n");

		// -- 2. one BackupManager = one service_mgr attachment ---------------
		BackupManager mgr{client, ServiceManagerOptions()
			.setServer("localhost")
			.setUserName(envOr("ISC_USER", "SYSDBA"))
			.setPassword(envOr("ISC_PASSWORD", "masterkey"))};

		// -- 3. gbak backup, verbose log streamed into a callback ------------
		printf("\n== backup: %s -> %s ==\n", SRC_PATH, FBK);
		mgr.backup(BackupOptions()
			.setDatabase(SRC_PATH)
			.setBackupFile(FBK)
			.setVerboseOutput(gbakLine));

		// -- 4. gbak restore (replace) through the same service --------------
		printf("\n== restore: %s -> %s ==\n", FBK, RES_PATH);
		mgr.restore(RestoreOptions()
			.setDatabase(RES_PATH)
			.setBackupFile(FBK)
			.setReplace(true)
			.setVerboseOutput(gbakLine));

		// -- 5. prove the restored copy has the data --------------------------
		Attachment rdb{client, DB_REST, defaultOptions()};
		Transaction rtra{rdb};
		Statement check{rdb, rtra, "SELECT COUNT(*), MAX(NAME) FROM BR_ITEMS"};
		check.execute(rtra);
		printf("\nrestored database says: %s rows, max name = %s\n",
			check.getString(0).value_or("?").c_str(),
			check.getString(1).value_or("?").c_str());
		rtra.commit();

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
