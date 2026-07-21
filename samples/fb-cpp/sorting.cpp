/*
 *  sorting.cpp (fb-cpp) — companion to ../../sorting-and-temp-space.md
 *
 *  The fb-cpp twin of ../cpp/sorting.cpp: the same 200,000-row table with
 *  a 400-byte key, the same big-vs-small ORDER BY pair straddling the
 *  64 MB TempCacheLimit, the same two-eyed watcher — the server's
 *  /proc/<pid>/fd table for unlinked fb_sort_* scratch files (plain
 *  popen; no wrapper can abstract /proc) and MON$MEMORY_USAGE polled from
 *  a second attachment, one fresh RAII Transaction per poll because MON$
 *  snapshots are per-transaction.  fb-cpp's contribution is the typed
 *  plumbing: MON$MEMORY_ALLOCATED arrives as std::optional<int64_t> from
 *  getInt64 instead of a string to atol, and the plans come prefetched
 *  via StatementOptions().setPrefetchLegacyPlan(true).
 *
 *  Needs to run on the server machine with passwordless sudo (to read
 *  /proc/<serverpid>/fd of the firebird-owned process).
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_sorting [database]
 */

#include "fbcpp_sample.h"
#include <atomic>
#include <string>
#include <thread>

using namespace fbcpp;
using namespace fbcpp_sample;

static std::atomic<bool> running{true};
static std::atomic<long> peakScratch{0}, peakFiles{0}, peakMem{0};

static const char* memSql =
	"select m.mon$memory_allocated from mon$database d "
	"join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id";

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

static void watcher(Client* client, const char* database, long pid)
{
	Attachment mon{*client, database, defaultOptions()};	// second attachment
	while (running)
	{
		sampleScratch(pid);
		Transaction t{mon};						// new tx => new MON$ snapshot
		Statement stmt{mon, t, memSql};
		stmt.execute(t);
		const long mem = (long) stmt.getInt64(0).value_or(0);
		t.commit();
		if (mem > peakMem) peakMem = mem;
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/sorting_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		{
			Transaction t{att};
			att.execute(t, "recreate table bulk (id integer, pad varchar(400) character set ascii)");
			t.commit();
		}
		{
			Transaction t{att};
			att.execute(t,
				"execute block as declare i integer = 0; begin"
				"  while (i < 200000) do begin"
				"    insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x'));"
				"    i = i + 1;"
				"  end "
				"end");
			t.commit();
		}
		printf("bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data\n");

		long pid, memIdle;
		{
			Transaction t{att};
			Statement s1{att, t, "select mon$server_pid from mon$attachments "
				"where mon$attachment_id = current_connection"};
			s1.execute(t);
			pid = s1.getInt32(0).value_or(0);
			Statement s2{att, t, memSql};
			s2.execute(t);
			memIdle = (long) s2.getInt64(0).value_or(0);
			t.commit();
		}
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
			std::thread w(watcher, &client, database, pid);

			Transaction tra{att};
			Statement stmt{att, tra, c.sql, StatementOptions().setPrefetchLegacyPlan(true)};
			std::string plan = stmt.getLegacyPlan();
			plan.erase(0, plan.find_first_not_of('\n'));
			printf("\n%s\n  %s\n", c.label, plan.c_str());
			stmt.execute(tra);					// the sort happens here
			const int top = stmt.getInt32(0).value_or(-1);
			tra.commit();

			running = false;
			w.join();
			printf("  top row id = %d\n", top);
			printf("  peak fb_sort_* scratch: %ld file(s), %ld bytes\n",
				peakFiles.load(), peakScratch.load());
			printf("  peak database MON$MEMORY_ALLOCATED: %ld bytes (+%ld over idle)\n",
				peakMem.load(), peakMem.load() - memIdle);
		}

		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
