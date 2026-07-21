/*
 *  services.cpp — the Services API from client code: IService, an SPB-driven
 *  action, and the 1 KB ring-buffer polling loop.
 *
 *  Companion to ../../services-api.md.  The sample attaches to the reserved
 *  service_mgr name, asks the server for its version, then starts a VERBOSE
 *  backup of a scratch database.  The backup runs BURP_main — the real gbak —
 *  on a server thread; its output arrives through the 1 KB svc_stdout ring
 *  buffer, drained here with repeated IService::query(isc_info_svc_line)
 *  calls.  Every path in the SPB is a SERVER path: the .fbk lands on the
 *  server's filesystem, owned by the server's user, and this client never
 *  touches either file.
 */

#include "fb_sample.h"

using namespace Firebird;

// One isc_info_svc_line answer: 2-byte little-endian length + text.
// Returns false on the empty line that means "utility finished".
static bool printLines(const unsigned char* p, unsigned size, int& lines)
{
	const unsigned char* end = p + size;
	bool more = false;
	while (p < end && *p != isc_info_end)
	{
		switch (*p++)
		{
		case isc_info_svc_line:
		{
			unsigned len = (unsigned) isc_portable_integer(p, 2);
			p += 2;
			if (len > 0)
			{
				printf("  %.*s\n", (int) len, p);
				++lines;
				more = true;
			}
			p += len;
			break;
		}
		case isc_info_svc_timeout:
		case isc_info_data_not_ready:
			more = true;
			break;
		case isc_info_truncated:
			more = true;
			break;
		default:
			return more;
		}
	}
	return more;
}

int main(int argc, char** argv)
{
	const char* dbPath  = fbsample::argOrDefault(argc, argv, 1, "/tmp/fbhandson/services.fdb");
	const char* bkPath  = fbsample::argOrDefault(argc, argv, 2, "/tmp/fbhandson/services.fbk");
	const std::string dsn = std::string("inet://localhost/") + dbPath;

	try
	{
		// 0. Make sure the scratch database exists (idempotent).
		{
			fbsample::Db db;
			db.attachOrCreate(dsn.c_str());
			ITransaction* tra = db.start();
			try { db.exec(tra, "create table t (id int, v varchar(20))"); }
			catch (const FbException&) {}   // already there
			tra->commit(&db.status);
		}

		IMaster* master = fbsample::master;
		ThrowStatusWrapper st(master->getStatus());
		IProvider* prov = master->getDispatcher();
		IUtil* util = master->getUtilInterface();

		// 1. Attach to the service manager: credentials in an SPB_ATTACH.
		IXpbBuilder* spb = util->getXpbBuilder(&st, IXpbBuilder::SPB_ATTACH, nullptr, 0);
		spb->insertString(&st, isc_spb_user_name, fbsample::Db::defaultUser());
		spb->insertString(&st, isc_spb_password, fbsample::Db::defaultPassword());
		IService* svc = prov->attachServiceManager(&st, "localhost:service_mgr",
			spb->getBufferLength(&st), spb->getBuffer(&st));
		spb->dispose();

		// 2. Information request: no action, just server facts.
		const unsigned char verItems[] = { isc_info_svc_server_version };
		unsigned char results[1024];
		svc->query(&st, 0, nullptr, sizeof(verItems), verItems, sizeof(results), results);
		if (results[0] == isc_info_svc_server_version)
		{
			unsigned len = (unsigned) isc_portable_integer(results + 1, 2);
			printf("server version: %.*s\n", (int) len, results + 3);
		}

		// 3. Start action_backup — the services[] table dispatches this to
		//    BURP_main, i.e. gbak itself, on a server thread.
		IXpbBuilder* start = util->getXpbBuilder(&st, IXpbBuilder::SPB_START, nullptr, 0);
		start->insertTag(&st, isc_action_svc_backup);
		start->insertString(&st, isc_spb_dbname, dbPath);      // server path!
		start->insertString(&st, isc_spb_bkp_file, bkPath);    // server path!
		start->insertTag(&st, isc_spb_verbose);
		svc->start(&st, start->getBufferLength(&st), start->getBuffer(&st));
		start->dispose();
		printf("backup started (verbose) — draining the 1 KB ring buffer:\n");

		// 4. The polling loop: each query() drains at most one buffer's worth
		//    of gbak output; the producer BLOCKS whenever the buffer is full,
		//    so a client that stops polling stalls the backup.
		const unsigned char lineItems[] = { isc_info_svc_line };
		int lines = 0, polls = 0;
		bool more = true;
		while (more)
		{
			svc->query(&st, 0, nullptr, sizeof(lineItems), lineItems,
				sizeof(results), results);
			++polls;
			more = printLines(results, sizeof(results), lines);
		}
		printf("done: %d gbak lines drained in %d query() polls\n", lines, polls);
		printf("the file %s now exists on the SERVER, owned by the server's user\n", bkPath);

		svc->detach(&st);
		prov->release();
		st.dispose();
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
	return 0;
}
