/*
 *  page_cache.cpp — companion sample for page-cache-coherency.md.
 *
 *  Runs the same hot-page ping-pong twice and lets MON$IO_STATS tell the
 *  difference between the two cache topologies:
 *
 *    phase 1 — two client processes -> ONE SuperServer shared cache
 *              (coherency by shared memory; almost no physical I/O)
 *    phase 2 — two EMBEDDED engine processes with PRIVATE caches over one
 *              file (ServerMode=SuperClassic sandbox; coherency by LCK_bdb
 *              page locks + blocking ASTs — data travels through the disk,
 *              so the same workload turns into hundreds of reads AND writes)
 *
 *  The parent only forks/execs; every attachment lives in a child process
 *  (each embedded child IS a full engine).  Rows 1 and 2 share a data page,
 *  so the two writers fight over one page without ever touching one row.
 */

#include "fb_sample.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace fbsample;

static const char* SRV_DB = "inet://localhost//tmp/fbhandson/page_cache_srv.fdb";
static const char* EMB_DB = "/tmp/fbhandson/page_cache_emb.fdb";
static const char* SANDBOX = "/tmp/fbhandson/fbemb";
static const int ROUNDS = 300;

static int initDb(const char* conn)          // --init: fresh table, two rows
{
	Db db;
	db.attachOrCreate(conn);
	ITransaction* t = db.start();
	try { db.exec(t, "drop table t"); } catch (const FbException&) {}
	t->commit(&db.status);
	t = db.start();
	db.exec(t, "create table t (id int primary key, v int)");
	t->commit(&db.status);
	t = db.start();
	db.exec(t, "insert into t values (1, 0)");
	db.exec(t, "insert into t values (2, 0)");
	t->commit(&db.status);
	return 0;
}

static int worker(const char* conn, const char* rowId)   // --worker
{
	Db db;
	db.attach(conn);
	for (int i = 0; i < ROUNDS; ++i)
	{
		ITransaction* t = db.start();
		db.exec(t, std::string("update t set v = v + 1 where id = ") + rowId);
		t->commit(&db.status);
	}
	ITransaction* t = db.start();
	Db::Table io = db.query(t,
		"select MON$PAGE_FETCHES, MON$PAGE_READS, MON$PAGE_WRITES "
		"from MON$IO_STATS join MON$ATTACHMENTS using (MON$STAT_ID) "
		"where MON$ATTACHMENT_ID = CURRENT_CONNECTION");
	printf("  worker pid %-6d row %s: %d commits | page fetches=%-6s "
		"reads=%-4s writes=%s\n", getpid(), rowId, ROUNDS,
		io.rows[0][0].c_str(), io.rows[0][1].c_str(), io.rows[0][2].c_str());
	t->commit(&db.status);
	return 0;
}

static int check(const char* conn)           // --check: any lost updates?
{
	Db db;
	db.attach(conn);
	ITransaction* t = db.start();
	Db::Table r = db.query(t, "select id, v from t order by id");
	for (auto& row : r.rows)
		printf("  final: id=%s v=%s (expected %d)\n",
			row[0].c_str(), row[1].c_str(), ROUNDS);
	t->commit(&db.status);
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
	catch (const FbException& e) { return report(e); }
}
