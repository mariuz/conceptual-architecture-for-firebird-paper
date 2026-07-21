/*
 *  trace.cpp (fb-cpp) — a live trace session, typed where fb-cpp reaches.
 *
 *  The fb-cpp twin of ../cpp/trace.cpp: service A starts a user trace
 *  session with an inline configuration and streams its TraceLog back; a
 *  worker attaches (its own Client + Attachment, fb-cpp's per-thread idiom)
 *  and runs one marker query — the observed side; service B stops the
 *  session by id.  fb-cpp wraps service ACTIONS (backup, restore, repair),
 *  not the trace family, so this twin shows the layered escape hatch at
 *  full stretch: ServiceManager owns the attach/detach lifecycle (SPB_ATTACH
 *  built from typed options, RAII detach), while trace_start/query/trace_stop
 *  drop to the underlying IService via getHandle() — the same raw tags as
 *  the OO-API version, inside an otherwise typed program.
 *  See ../../trace-and-audit.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_trace
 */

#include "fbcpp_sample.h"
#include <ibase.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DB_PATH = "/tmp/fbhandson/trace_fbcpp.fdb";

static const char* TRACE_CFG =
	"database = /tmp/fbhandson/trace_fbcpp.fdb\n"
	"{\n"
	"  enabled = true\n"
	"  log_connections = true\n"
	"  log_transactions = true\n"
	"  log_statement_finish = true\n"
	"  print_plan = true\n"
	"  print_perf = true\n"
	"  time_threshold = 0\n"
	"}\n";

static ServiceManagerOptions svcOptions()
{
	return ServiceManagerOptions()
		.setServer("localhost")
		.setUserName(envOr("ISC_USER", "SYSDBA"))
		.setPassword(envOr("ISC_PASSWORD", "masterkey"));
}

// Fetch one service output line; returns false when the stream is finished.
static bool nextLine(fb::CheckStatusWrapper& st, fb::IService* svc, std::string& line)
{
	const unsigned char items[] = { isc_info_svc_line };
	unsigned char buf[4096];
	svc->query(&st, 0, nullptr, sizeof(items), items, sizeof(buf), buf);
	if ((st.getState() & fb::IStatus::STATE_ERRORS) || buf[0] != isc_info_svc_line)
		return false;
	const unsigned len = buf[1] | (unsigned(buf[2]) << 8);
	line.assign(reinterpret_cast<char*>(buf + 3), len);
	return len != 0;
}

int main()
{
	try
	{
		Client client{"fbclient"};

		// The database to be observed must exist before the session starts.
		attachOrCreate(client, std::string("inet://localhost/") + DB_PATH);

		fb::IMaster* master = client.getMaster();
		fb::IUtil* utl = master->getUtilInterface();

		// -- service A: typed attach, then trace_start through the handle ---
		ServiceManager svcA{client, svcOptions()};
		fb::CheckStatusWrapper st{master->getStatus()};
		fb::IXpbBuilder* start = utl->getXpbBuilder(&st, fb::IXpbBuilder::SPB_START, nullptr, 0);
		start->insertTag(&st, isc_action_svc_trace_start);
		start->insertString(&st, isc_spb_trc_name, "hands-on (fb-cpp)");
		start->insertString(&st, isc_spb_trc_cfg, TRACE_CFG);
		svcA.getHandle()->start(&st, start->getBufferLength(&st), start->getBuffer(&st));
		start->dispose();
		if (st.getState() & fb::IStatus::STATE_ERRORS)
			throw FbCppException("trace_start failed");

		std::atomic<int> sessionId{0};

		// -- worker: the observed side, then the stop from a second service -
		std::thread worker([&sessionId]()
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(800));
			try
			{
				Client c{"fbclient"};
				Attachment att{c, std::string("inet://localhost/") + DB_PATH,
					defaultOptions()};
				Transaction tra{att};
				Statement marker{att, tra,
					"SELECT COUNT(*) FROM RDB$RELATIONS /* traced! */"};
				marker.execute(tra);
				printf("[worker] marker query says: %s\n",
					marker.getString(0)->c_str());
				tra.commit();
			}
			catch (const std::exception& e) { report(e); }

			std::this_thread::sleep_for(std::chrono::milliseconds(1200));
			while (!sessionId.load())                   // wait for the id from the stream
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			try
			{
				Client c{"fbclient"};
				ServiceManager svcB{c, svcOptions()};
				fb::CheckStatusWrapper st2{c.getMaster()->getStatus()};
				fb::IUtil* utl2 = c.getMaster()->getUtilInterface();
				fb::IXpbBuilder* stop = utl2->getXpbBuilder(&st2, fb::IXpbBuilder::SPB_START, nullptr, 0);
				stop->insertTag(&st2, isc_action_svc_trace_stop);
				stop->insertInt(&st2, isc_spb_trc_id, sessionId.load());
				svcB.getHandle()->start(&st2, stop->getBufferLength(&st2), stop->getBuffer(&st2));
				stop->dispose();
				std::string line;
				while (nextLine(st2, svcB.getHandle().get(), line))
					printf("[stop ] %s\n", line.c_str());
				st2.dispose();
			}
			catch (const std::exception& e) { report(e); }
		});

		// -- service A's stream: the trace output itself --------------------
		std::string line;
		while (nextLine(st, svcA.getHandle().get(), line))
		{
			printf("[trace] %s\n", line.c_str());
			int id;
			if (sscanf(line.c_str(), "Trace session ID %d started", &id) == 1)
				sessionId.store(id);
		}

		worker.join();
		st.dispose();
		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
