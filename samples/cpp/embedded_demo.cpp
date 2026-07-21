/*
 *  embedded_demo.cpp — the full server engine, loaded into this process.
 *
 *  Companion sample for ../../embedded-architecture-comparison.md.  Three
 *  demonstrations in one program:
 *
 *    1. libfbclient is client AND engine: before the first local-path attach
 *       only libfbclient.so is mapped into the process; the attach makes the
 *       Y-valve load the Engine provider (plugins/libEngine14.so), and the
 *       process memory map proves it.
 *    2. Real work, no server: CREATE TABLE / INSERT / SELECT against a local
 *       .fdb created by this process — DSQL, JRD, MVCC and careful write all
 *       running on our own call stack (NETWORK_PROTOCOL is NULL).
 *    3. The continuum is measurable: attach/detach timed embedded vs remote —
 *       same API, same engine; the difference is the socket, the SRP
 *       handshake and a server round-trip per call.
 */

#include "fb_sample.h"
#include <chrono>
#include <unistd.h>

using namespace fbsample;

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

static double attachMs(const char* database)
{
	auto t0 = std::chrono::steady_clock::now();
	{
		Db db;
		db.attach(database);
	}	// detach in ~Db()
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main(int argc, char** argv)
{
	const char* localDb = argOrDefault(argc, argv, 1,
		"/tmp/fbhandson/embedded_demo.fdb");
	const char* remoteDb = argOrDefault(argc, argv, 2,
		"inet://localhost/employee");

	try
	{
		// --- 1. watch the engine arrive in our address space -------------
		printf("before attach: libfbclient mapped=%s, libEngine14 mapped=%s\n",
			mapped("libfbclient") ? "yes" : "no",
			mapped("libEngine14") ? "yes" : "no");

		Db db;
		db.attachOrCreate(localDb);

		printf("after  attach: libfbclient mapped=%s, libEngine14 mapped=%s\n\n",
			mapped("libfbclient") ? "yes" : "no",
			mapped("libEngine14") ? "yes" : "no");

		// --- 2. real work with no server anywhere ------------------------
		ITransaction* tra = db.start();
		try { db.exec(tra, "drop table gadgets"); } catch (const FbException&) {}
		db.exec(tra, "create table gadgets (id int primary key, name varchar(20))");
		tra->commitRetaining(&db.status);
		db.exec(tra, "insert into gadgets values (1, 'sprocket')");
		db.exec(tra, "insert into gadgets values (2, 'flange')");
		db.exec(tra, "insert into gadgets values (3, 'grommet')");
		tra->commitRetaining(&db.status);

		Db::Table t = db.query(tra,
			"select count(*), max(name), "
			"       coalesce(rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), "
			"                '<null: in-process>'), "
			"       a.mon$server_pid "
			"from gadgets, mon$attachments a "
			"where a.mon$attachment_id = current_connection "
			"group by 3, 4");
		printf("rows=%s  max(name)=%s  NETWORK_PROTOCOL=%s\n",
			t.rows[0][0].c_str(), t.rows[0][1].c_str(), t.rows[0][2].c_str());
		printf("engine pid=%s, my pid=%d — the 'server' is this process\n\n",
			t.rows[0][3].c_str(), getpid());
		tra->commit(&db.status);

		// --- 3. attach cost: in-process call vs socket + SRP handshake ---
		const int runs = 5;
		double emb = 0, rem = 0;
		attachMs(localDb);		// warm-up (provider already loaded anyway)
		attachMs(remoteDb);		// warm-up (socket/auth code paths)
		for (int i = 0; i < runs; ++i)
		{
			emb += attachMs(localDb);
			rem += attachMs(remoteDb);
		}
		printf("attach+detach avg over %d runs:\n", runs);
		printf("    embedded  %-38s %7.2f ms\n", localDb, emb / runs);
		printf("    remote    %-38s %7.2f ms\n", remoteDb, rem / runs);
		printf("done.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
