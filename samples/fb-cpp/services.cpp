/*
 *  services.cpp (fb-cpp) — the Services API as typed actions: BackupManager
 *  and the verbose-output callback.
 *
 *  The fb-cpp twin of ../cpp/services.cpp.  The OO-API version assembles the
 *  SPB_ATTACH and SPB_START tag by tag and hand-runs the 1 KB ring-buffer
 *  polling loop (isc_info_svc_line, one query() per line).  fb-cpp models
 *  the same session as objects: ServiceManagerOptions replaces SPB_ATTACH,
 *  BackupOptions replaces the SPB_START tags, and the polling loop lives
 *  inside ServiceManager::waitForCompletion — each drained gbak line
 *  surfaces through a std::function callback.  fb-cpp wraps service ACTIONS
 *  (backup, restore, properties, repair), not raw info queries, so the
 *  server-version request drops to the underlying IService handle — the
 *  escape hatch getHandle() exists for.  Every path in the options is still
 *  a SERVER path.  See ../../services-api.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_services [db-server-path] [fbk-server-path]
 */

#include "fbcpp_sample.h"
#include <ibase.h>
#include <cstdio>
#include <string>
#include <string_view>

using namespace fbcpp;
using namespace fbcpp_sample;

int main(int argc, char** argv)
{
	const char* dbPath = argOrDefault(argc, argv, 1, "/tmp/fbhandson/services_fbcpp.fdb");
	const char* bkPath = argOrDefault(argc, argv, 2, "/tmp/fbhandson/services_fbcpp.fbk");
	const std::string dsn = std::string("inet://localhost/") + dbPath;

	try
	{
		Client client{"fbclient"};

		// 0. Make sure the scratch database exists (idempotent).
		{
			Attachment att = attachOrCreate(client, dsn);
			Transaction tra{att};
			try
			{
				att.execute(tra, "create table t (id int, v varchar(20))");
			}
			catch (const DatabaseException&)
			{
				// already there
			}
			tra.commit();
		}

		// 1. Attach to the service manager: typed options instead of SPB_ATTACH.
		BackupManager svc{client, ServiceManagerOptions()
			.setServer("localhost")
			.setUserName(envOr("ISC_USER", "SYSDBA"))
			.setPassword(envOr("ISC_PASSWORD", "masterkey"))};

		// 2. Information request — no fb-cpp wrapper, so use the raw handle.
		{
			fb::CheckStatusWrapper st{client.getMaster()->getStatus()};
			const unsigned char items[] = { isc_info_svc_server_version };
			unsigned char results[1024];
			svc.getHandle()->query(&st, 0, nullptr, sizeof(items), items,
				sizeof(results), results);
			if (st.getState() & fb::IStatus::STATE_ERRORS)
				throw FbCppException("service version query failed");
			if (results[0] == isc_info_svc_server_version)
			{
				// isc_portable_integer lives in fbclient, which these samples
				// never link against — decode the little-endian word locally.
				const unsigned len = results[1] | (unsigned(results[2]) << 8);
				printf("server version: %.*s\n", (int) len, results + 3);
			}
			st.dispose();
		}

		// 3+4. The verbose backup.  backup() builds the SPB_START
		//      (isc_action_svc_backup, dbname, bkp_file, isc_spb_verbose) and
		//      runs the ring-buffer polling loop; the callback fires once per
		//      drained gbak line.
		printf("backup started (verbose) — gbak lines via the callback:\n");
		int lines = 0;
		svc.backup(BackupOptions()
			.setDatabase(dbPath)         // server path!
			.setBackupFile(bkPath)       // server path!
			.setVerboseOutput([&lines](std::string_view line)
			{
				printf("  %.*s\n", (int) line.size(), line.data());
				++lines;
			}));
		printf("done: %d gbak lines streamed through the callback\n", lines);
		printf("the file %s now exists on the SERVER, owned by the server's user\n", bkPath);

		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
