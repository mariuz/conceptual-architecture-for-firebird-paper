/*
 *  page_cache.cpp (fb-cpp) — one shared cache vs two private caches.
 *
 *  The fb-cpp twin of ../cpp/page_cache.cpp: the same hot-page ping-pong
 *  run against both cache topologies (phase 1: two clients of one
 *  SuperServer shared cache; phase 2: two EMBEDDED engine processes with
 *  private caches over one file, via a SuperClassic sandbox this program
 *  builds itself), refereed by per-attachment MON$IO_STATS.  fb-cpp fetches
 *  each worker's I/O counters straight into a struct with
 *  queryFirstRowAs<IoStats>(); the fork/exec choreography is identical —
 *  cache topology is decided by the connection string, not the wrapper.
 *  See ../../page-cache-coherency.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_page_cache
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* SRV_DB = "inet://localhost//tmp/fbhandson/page_cache_srv_fbcpp.fdb";
static const char* EMB_DB = "/tmp/fbhandson/page_cache_emb_fbcpp.fdb";
static const char* SANDBOX = "/tmp/fbhandson/fbemb_fbcpp";
static const int ROUNDS = 300;

struct IoStats
{
	std::int64_t fetches, reads, writes;
};

static int initDb(const char* conn)          // --init: fresh table, two rows
{
	Client client{"fbclient"};
	Attachment att = attachOrCreate(client, conn);
	{
		Transaction t{att};
		try { att.execute(t, "drop table t"); }
		catch (const DatabaseException&) {}
		t.commit();
	}
	Transaction ddl{att};
	att.execute(ddl, "create table t (id int primary key, v int)");
	ddl.commit();
	Transaction ins{att};
	att.execute(ins, "insert into t values (1, 0)");
	att.execute(ins, "insert into t values (2, 0)");
	ins.commit();
	return 0;
}

static int worker(const char* conn, const char* rowId)   // --worker
{
	Client client{"fbclient"};
	Attachment att{client, conn, defaultOptions()};
	const std::string update = std::string("update t set v = v + 1 where id = ") + rowId;
	for (int i = 0; i < ROUNDS; ++i)
	{
		Transaction t{att};
		att.execute(t, update);
		t.commit();
	}
	Transaction t{att};
	const IoStats io = att.queryFirstRowAs<IoStats>(t,
		"select MON$PAGE_FETCHES, MON$PAGE_READS, MON$PAGE_WRITES "
		"from MON$IO_STATS join MON$ATTACHMENTS using (MON$STAT_ID) "
		"where MON$ATTACHMENT_ID = CURRENT_CONNECTION").value();
	printf("  worker pid %-6d row %s: %d commits | page fetches=%-6lld "
		"reads=%-4lld writes=%lld\n", getpid(), rowId, ROUNDS,
		(long long) io.fetches, (long long) io.reads, (long long) io.writes);
	t.commit();
	return 0;
}

static int check(const char* conn)           // --check: any lost updates?
{
	Client client{"fbclient"};
	Attachment att{client, conn, defaultOptions()};
	Transaction t{att};
	Statement stmt{att, t, "select id, v from t order by id"};
	for (bool ok = stmt.execute(t); ok; ok = stmt.fetchNext())
		printf("  final: id=%d v=%d (expected %d)\n",
			stmt.getInt32(0).value_or(-1), stmt.getInt32(1).value_or(-1), ROUNDS);
	t.commit();
	return 0;
}

static void run(const char* self, std::initializer_list<const char*> args)
{
	if (fork() == 0)
	{
		std::vector<const char*> v{self};
		v.insert(v.end(), args.begin(), args.end());
		v.push_back(nullptr);
		execv(self, const_cast<char**>(v.data()));
		_exit(127);
	}
}

static void await() { while (wait(nullptr) > 0) {} }

int main(int argc, char** argv)
{
	try
	{
		if (argc > 2 && !strcmp(argv[1], "--init"))   return initDb(argv[2]);
		if (argc > 3 && !strcmp(argv[1], "--worker")) return worker(argv[2], argv[3]);
		if (argc > 2 && !strcmp(argv[1], "--check"))  return check(argv[2]);

		printf("phase 1: two client processes, ONE SuperServer shared cache\n");
		fflush(stdout);
		run(argv[0], {"--init", SRV_DB}); await();
		run(argv[0], {"--worker", SRV_DB, "1"});
		run(argv[0], {"--worker", SRV_DB, "2"});
		await();
		run(argv[0], {"--check", SRV_DB}); await();

		// Build the embedded sandbox: a FIREBIRD root whose firebird.conf
		// says SuperClassic, so each process locks the file SHARED and runs
		// its own page cache (see 'Three layers of arbitration').
		mkdir(SANDBOX, 0777);
		for (const char* f : {"plugins", "intl", "tzdata", "firebird.msg", "security6.fdb"})
			symlink((std::string("/opt/firebird/") + f).c_str(),
				(std::string(SANDBOX) + "/" + f).c_str());
		FILE* conf = fopen((std::string(SANDBOX) + "/firebird.conf").c_str(), "w");
		fputs("ServerMode = SuperClassic\n", conf);
		fclose(conf);
		setenv("FIREBIRD", SANDBOX, 1);
		unlink(EMB_DB);

		printf("phase 2: two EMBEDDED engine processes, PRIVATE page caches\n");
		fflush(stdout);
		run(argv[0], {"--init", EMB_DB}); await();
		run(argv[0], {"--worker", EMB_DB, "1"});
		run(argv[0], {"--worker", EMB_DB, "2"});
		await();
		run(argv[0], {"--check", EMB_DB}); await();
		printf("same workload — the private caches paid for coherency in disk I/O.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
