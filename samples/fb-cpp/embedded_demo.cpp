/*
 *  embedded_demo.cpp (fb-cpp) — the engine arriving in this address space.
 *
 *  The fb-cpp twin of ../cpp/embedded_demo.cpp, with one extra rung on the
 *  ladder: fb-cpp's Client{"fbclient"} loads even the CLIENT library at
 *  runtime (Boost.DLL), so /proc/self/maps shows THREE states instead of
 *  two — before Client{} not even libfbclient is mapped; after Client{}
 *  the client is mapped but the engine is not; after the first local-path
 *  attach the Y-valve has pulled the full server (libEngine14.so) into
 *  the process.  Then the same real work with no server (DDL, DML,
 *  NETWORK_PROTOCOL NULL, MON$SERVER_PID == getpid()) and the same
 *  attach+detach timing, embedded vs remote.
 *  See ../../embedded-architecture-comparison.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_embedded_demo [local-path] [remote-db]
 */

#include "fbcpp_sample.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace fbcpp;
using namespace fbcpp_sample;

// Is a shared object whose name contains `frag` mapped into this process?
static bool mapped(const char* frag)
{
	FILE* f = fopen("/proc/self/maps", "r");
	if (!f)
		return false;
	char line[512];
	bool found = false;
	while (fgets(line, sizeof line, f))
		if (strstr(line, frag))
			{ found = true; break; }
	fclose(f);
	return found;
}

static void printMapped(const char* when)
{
	printf("%s: libfbclient mapped=%s, libEngine14 mapped=%s\n", when,
		mapped("libfbclient") ? "yes" : "no",
		mapped("libEngine14") ? "yes" : "no");
}

static double attachMs(Client& client, const std::string& database)
{
	auto t0 = std::chrono::steady_clock::now();
	{
		Attachment att{client, database, defaultOptions()};
	}	// detach in ~Attachment()
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main(int argc, char** argv)
{
	const char* localDb = argOrDefault(argc, argv, 1,
		"/tmp/fbhandson/embedded_fbcpp.fdb");
	const char* remoteDb = argOrDefault(argc, argv, 2,
		"inet://localhost/employee");

	try
	{
		// --- 1. watch first the client, then the engine, arrive ----------
		printMapped("before Client{}    ");
		Client client{"fbclient"};		// Boost.DLL loads libfbclient now
		printMapped("after  Client{}    ");

		Attachment att = attachOrCreate(client, localDb);
		printMapped("after  local attach");
		printf("\n");

		// --- 2. real work with no server anywhere ------------------------
		Transaction tra{att};
		try { att.execute(tra, "drop table gadgets"); } catch (const DatabaseException&) {}
		att.execute(tra, "create table gadgets (id int primary key, name varchar(20))");
		tra.commitRetaining();
		att.execute(tra, "insert into gadgets values (1, 'sprocket')");
		att.execute(tra, "insert into gadgets values (2, 'flange')");
		att.execute(tra, "insert into gadgets values (3, 'grommet')");
		tra.commitRetaining();

		Statement stmt{att, tra,
			"select count(*), max(name), "
			"       rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), "
			"       a.mon$server_pid "
			"from gadgets, mon$attachments a "
			"where a.mon$attachment_id = current_connection "
			"group by 3, 4"};
		stmt.execute(tra);
		printf("rows=%lld  max(name)=%s  NETWORK_PROTOCOL=%s\n",
			static_cast<long long>(stmt.getInt64(0).value_or(0)),
			stmt.getString(1).value_or("<null>").c_str(),
			stmt.getString(2).value_or("<null: in-process>").c_str());
		printf("engine pid=%d, my pid=%d — the 'server' is this process\n\n",
			stmt.getInt32(3).value_or(-1), getpid());
		tra.commit();

		// --- 3. attach cost: in-process call vs socket + SRP handshake ---
		const int runs = 5;
		double emb = 0, rem = 0;
		attachMs(client, localDb);		// warm-up (provider already loaded anyway)
		attachMs(client, remoteDb);		// warm-up (socket/auth code paths)
		for (int i = 0; i < runs; ++i)
		{
			emb += attachMs(client, localDb);
			rem += attachMs(client, remoteDb);
		}
		printf("attach+detach avg over %d runs:\n", runs);
		printf("    embedded  %-38s %7.2f ms\n", localDb, emb / runs);
		printf("    remote    %-38s %7.2f ms\n", remoteDb, rem / runs);
		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
