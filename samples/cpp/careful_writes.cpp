/*
 *  careful_writes.cpp — companion sample for careful-writes-and-crash-safety.md.
 *
 *  Kills a database engine mid-write and shows the file needs no recovery.
 *
 *  Uses the EMBEDDED engine (attach by plain local path, FIREBIRD=/opt/firebird)
 *  so that the child process this program forks IS the engine: SIGKILLing it
 *  while an uncommitted bulk insert is flushing dirty pages is a genuine
 *  engine crash, not just a dropped client connection.  The parent then
 *  re-attaches and verifies the careful-write guarantee: committed rows all
 *  present, uncommitted rows all gone, attach instantaneous — no log replay,
 *  because there is no log.
 *
 *  Run it against a scratch path only:  ./careful_writes [/tmp/fbhandson/careful_writes.fdb]
 */

#include "fb_sample.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <chrono>

using namespace fbsample;
using clk = std::chrono::steady_clock;

static const char* DEFAULT_DB = "/tmp/fbhandson/careful_writes.fdb";

static long fileSize(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 ? long(st.st_size) : -1;
}

// Child mode: create the database, commit a marker, then bulk-insert half a
// million rows WITHOUT committing — and wait to be killed.
static int writer(const char* dbFile)
{
	Db db;
	db.attachOrCreate(dbFile);

	ITransaction* t = db.start();
	db.exec(t, "create table cw (id int, tag varchar(30))");
	t->commit(&db.status);

	t = db.start();
	db.exec(t, "insert into cw values (1, 'committed-marker')");
	t->commit(&db.status);
	printf("[writer %d] marker row committed (forced writes on)\n", getpid());
	fflush(stdout);

	t = db.start();          // never committed: the crash victim
	db.exec(t,
		"execute block as declare i int = 0; begin"
		"  while (i < 500000) do begin"
		"    insert into cw values (:i + 1000, 'uncommitted'); i = i + 1;"
		"  end "
		"end");
	printf("[writer] bulk insert finished uncommitted; waiting for SIGKILL\n");
	fflush(stdout);
	pause();
	return 0;
}

int main(int argc, char** argv)
{
	const bool childMode = argc > 1 && strcmp(argv[1], "--writer") == 0;
	const char* dbFile = argOrDefault(argc, argv, childMode ? 2 : 1, DEFAULT_DB);
	try
	{
		if (childMode)
			return writer(dbFile);

		unlink(dbFile);                       // fresh run
		setenv("FIREBIRD", "/opt/firebird", 0);

		// 1. Spawn the writer: a separate process running the embedded engine.
		pid_t pid = fork();
		if (pid == 0)
		{
			execl(argv[0], argv[0], "--writer", dbFile, (char*) nullptr);
			_exit(127);
		}

		// 2. Wait until the file is visibly growing — the engine is flushing
		//    freshly allocated data pages of the *uncommitted* transaction —
		//    then kill -9 the engine process mid-flight.
		long base = -1;
		for (int i = 0; i < 600; ++i)
		{
			usleep(50 * 1000);
			long sz = fileSize(dbFile);
			if (base < 0 && sz > 0)
				base = sz;
			if (base > 0 && sz > base + 2 * 1024 * 1024)
			{
				printf("file grew %ld -> %ld bytes; SIGKILL to engine pid %d\n",
					base, sz, pid);
				break;
			}
		}
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);

		// 3. Re-attach immediately.  No recovery step exists to run: the
		//    precedence graph never let an inconsistent state reach disk.
		auto t0 = clk::now();
		Db db;
		db.attach(dbFile);
		ITransaction* t = db.start();
		std::string committed = db.queryValue(t,
			"select count(*) from cw where tag = 'committed-marker'");
		std::string uncommitted = db.queryValue(t,
			"select count(*) from cw where tag = 'uncommitted'");
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			clk::now() - t0).count();
		t->commit(&db.status);

		printf("re-attach + both counts took %lld ms\n", (long long) ms);
		printf("committed marker rows : %s   <- survived the crash\n", committed.c_str());
		printf("uncommitted rows      : %s   <- rolled back by visibility, not replay\n",
			uncommitted.c_str());
		return 0;
	}
	catch (const FbException& e) { return report(e); }
}
