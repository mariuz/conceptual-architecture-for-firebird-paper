/*
 *  trace.cpp — companion sample for ../../trace-and-audit.md
 *
 *  A complete user trace session driven through the Services API — the same
 *  path fbtracemgr takes, no tools needed on the client:
 *
 *    service A: isc_action_svc_trace_start with an inline configuration
 *               (isc_spb_trc_cfg); the service then streams the session's
 *               TraceLog back line by line (isc_info_svc_line);
 *    worker:    attaches to the traced database and runs one marker query —
 *               the observed side of the stream;
 *    service B: isc_action_svc_trace_stop ends the session, which ends A's
 *               stream.
 *
 *  The trace configuration targets exactly one database, so the stream shows
 *  only the worker's doings: ATTACH, START_TRANSACTION, the statement with
 *  plan and per-table counters, COMMIT, DETACH.
 */

#include "fb_sample.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace Firebird;
using fbsample::master;

static const char* DB  = "inet://localhost//tmp/fbhandson/trace.fdb";
static const char* SRV = "localhost:service_mgr";

static const char* TRACE_CFG =
	"database = /tmp/fbhandson/trace.fdb\n"
	"{\n"
	"  enabled = true\n"
	"  log_connections = true\n"
	"  log_transactions = true\n"
	"  log_statement_finish = true\n"
	"  print_plan = true\n"
	"  print_perf = true\n"
	"  time_threshold = 0\n"
	"}\n";

static IService* attachSvc(ThrowStatusWrapper& st, IProvider* prov, IUtil* utl)
{
	IXpbBuilder* spb = utl->getXpbBuilder(&st, IXpbBuilder::SPB_ATTACH, nullptr, 0);
	spb->insertString(&st, isc_spb_user_name, fbsample::Db::defaultUser());
	spb->insertString(&st, isc_spb_password, fbsample::Db::defaultPassword());
	IService* svc = prov->attachServiceManager(&st, SRV,
		spb->getBufferLength(&st), spb->getBuffer(&st));
	spb->dispose();
	return svc;
}

// Fetch one service output line; returns false when the stream is finished.
static bool nextLine(ThrowStatusWrapper& st, IService* svc, std::string& line)
{
	const unsigned char items[] = { isc_info_svc_line };
	unsigned char buf[4096];
	svc->query(&st, 0, nullptr, sizeof(items), items, sizeof(buf), buf);
	if (buf[0] != isc_info_svc_line)
		return false;
	const unsigned len = buf[1] | (buf[2] << 8);
	line.assign(reinterpret_cast<char*>(buf + 3), len);
	return len != 0;
}

int main()
{
	try
	{
		// The database to be observed must exist before the session starts.
		{
			fbsample::Db db;
			db.attachOrCreate(DB);
		}

		ThrowStatusWrapper st(master->getStatus());
		IProvider* prov = master->getDispatcher();
		IUtil* utl = master->getUtilInterface();

		// -- service A: start the trace session, config passed inline -------
		IService* svcA = attachSvc(st, prov, utl);
		IXpbBuilder* start = utl->getXpbBuilder(&st, IXpbBuilder::SPB_START, nullptr, 0);
		start->insertTag(&st, isc_action_svc_trace_start);
		start->insertString(&st, isc_spb_trc_name, "hands-on");
		start->insertString(&st, isc_spb_trc_cfg, TRACE_CFG);
		svcA->start(&st, start->getBufferLength(&st), start->getBuffer(&st));
		start->dispose();

		std::atomic<int> sessionId{0};

		// -- worker: the observed side, then the stop from a second service -
		std::thread worker([&sessionId, prov, utl]()
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(800));
			try
			{
				fbsample::Db db;
				db.attach(DB);
				ITransaction* tra = db.start();
				printf("[worker] marker query says: %s\n",
					db.queryValue(tra,
						"SELECT COUNT(*) FROM RDB$RELATIONS /* traced! */").c_str());
				tra->commit(&db.status);
			}
			catch (const FbException& e) { fbsample::report(e); }

			std::this_thread::sleep_for(std::chrono::milliseconds(1200));
			while (!sessionId.load())                   // wait for the id from the stream
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			try
			{
				ThrowStatusWrapper st2(fbsample::master->getStatus());
				IService* svcB = attachSvc(st2, prov, utl);
				IXpbBuilder* stop = utl->getXpbBuilder(&st2, IXpbBuilder::SPB_START, nullptr, 0);
				stop->insertTag(&st2, isc_action_svc_trace_stop);
				stop->insertInt(&st2, isc_spb_trc_id, sessionId.load());
				svcB->start(&st2, stop->getBufferLength(&st2), stop->getBuffer(&st2));
				stop->dispose();
				std::string line;
				while (nextLine(st2, svcB, line))
					printf("[stop ] %s\n", line.c_str());
				svcB->detach(&st2);
				st2.dispose();
			}
			catch (const FbException& e) { fbsample::report(e); }
		});

		// -- service A's stream: the trace output itself --------------------
		std::string line;
		while (nextLine(st, svcA, line))
		{
			printf("[trace] %s\n", line.c_str());
			int id;
			if (sscanf(line.c_str(), "Trace session ID %d started", &id) == 1)
				sessionId.store(id);
		}

		worker.join();
		svcA->detach(&st);
		prov->release();
		st.dispose();
		printf("done.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
}
