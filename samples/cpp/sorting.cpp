/*
 *  sorting.cpp — companion to ../../sorting-and-temp-space.md
 *
 *  The TempCacheLimit threshold made visible.  Fills a table with 200,000
 *  rows (~82 MB of sort data with a 400-byte key), then runs two ORDER BY
 *  queries that both get PLAN SORT (... NATURAL):
 *
 *    big sort   200,000 rows  ->  ~82 MB  >  TempCacheLimit (64 MB)  -> spills
 *    small sort  20,000 rows  ->   ~8 MB  <  TempCacheLimit          -> stays in RAM
 *
 *  While each query runs, a watcher thread samples two things:
 *    -  the server's /proc/<pid>/fd table (via sudo) for fb_sort_* scratch
 *       files — they are unlinked on creation, so the fd table is the ONLY
 *       place they are visible;
 *    -  MON$MEMORY_USAGE at database level (that is where TempSpace's
 *       cache is charged), from a second attachment — each poll runs in a
 *       fresh transaction, because MON$ snapshots are per-transaction.
 *
 *  Needs to run on the server machine with passwordless sudo (to read
 *  /proc/<serverpid>/fd of the firebird-owned process).
 */

#include "fb_sample.h"
#include <atomic>
#include <thread>

using namespace fbsample;

static std::atomic<bool> running{true};
static std::atomic<long> peakScratch{0}, peakFiles{0}, peakMem{0};

// Sum the sizes of the server's open (already-unlinked) fb_sort_* files.
static void sampleScratch(long pid)
{
	char cmd[256];
	snprintf(cmd, sizeof cmd,
		"sudo -n find /proc/%ld/fd -lname '*fb_sort*' -print0 2>/dev/null"
		" | xargs -0 -r sudo -n stat -L -c %%s 2>/dev/null", pid);
	FILE* p = popen(cmd, "r");
	if (!p)
		return;
	long total = 0, files = 0, sz;
	while (fscanf(p, "%ld", &sz) == 1)
		{ total += sz; ++files; }
	pclose(p);
	if (total > peakScratch) peakScratch = total;
	if (files > peakFiles) peakFiles = files;
}

static void watcher(const char* database, long pid)
{
	Db mon;										// second attachment: MON$ polling
	mon.attach(database);
	const char* sql =
		"select m.mon$memory_allocated from mon$database d "
		"join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id";
	while (running)
	{
		sampleScratch(pid);
		ITransaction* t = mon.start();			// new tx => new MON$ snapshot
		long mem = atol(mon.queryValue(t, sql).c_str());
		t->commit(&mon.status);
		if (mem > peakMem) peakMem = mem;
	}
	mon.att->detach(&mon.status);
	mon.att = nullptr;
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost//tmp/fbhandson/sorting.fdb");

	try
	{
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();
		db.exec(tra, "recreate table bulk (id integer, pad varchar(400) character set ascii)");
		tra->commit(&db.status);
		tra = db.start();
		db.exec(tra,
			"execute block as declare i integer = 0; begin"
			"  while (i < 200000) do begin"
			"    insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x'));"
			"    i = i + 1;"
			"  end "
			"end");
		tra->commit(&db.status);
		printf("bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data\n");

		tra = db.start();
		const long pid = atol(db.queryValue(tra,
			"select mon$server_pid from mon$attachments "
			"where mon$attachment_id = current_connection").c_str());
		const long memIdle = atol(db.queryValue(tra,
			"select m.mon$memory_allocated from mon$database d "
			"join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id").c_str());
		tra->commit(&db.status);
		printf("server pid %ld, database memory allocated while idle: %ld bytes\n",
			pid, memIdle);

		struct Case { const char* label; const char* sql; };
		const Case cases[] = {
			{ "big sort (200k rows, ~82 MB)",
			  "select first 1 id from bulk order by pad desc" },
			{ "small sort (20k rows, ~8 MB)",
			  "select first 1 id from bulk where mod(id, 10) = 0 order by pad desc" },
		};

		for (const Case& c : cases)
		{
			peakScratch = peakFiles = peakMem = 0;
			running = true;
			std::thread w(watcher, database, pid);

			tra = db.start();
			IStatement* stmt = db.att->prepare(&db.status, tra, 0, c.sql, SQL_DIALECT_V6, 0);
			printf("\n%s\n  %s\n", c.label, stmt->getPlan(&db.status, false) + 1);
			std::string top = db.queryValue(tra, c.sql);	// the sort happens here
			tra->commit(&db.status);
			stmt->free(&db.status);

			running = false;
			w.join();
			printf("  top row id = %s\n", top.c_str());
			printf("  peak fb_sort_* scratch: %ld file(s), %ld bytes\n",
				peakFiles.load(), peakScratch.load());
			printf("  peak database MON$MEMORY_ALLOCATED: %ld bytes (+%ld over idle)\n",
				peakMem.load(), peakMem.load() - memIdle);
		}

		db.att->detach(&db.status);
		db.att = nullptr;
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& error)
	{
		return report(error);
	}
}
