/*
 *  careful_writes.cpp (fb-cpp) — kill the engine mid-write, count survivors.
 *
 *  The fb-cpp twin of ../cpp/careful_writes.cpp, same trick included: the
 *  child attaches by plain local path with FIREBIRD=/opt/firebird, so the
 *  EMBEDDED engine runs inside the process being SIGKILLed — a genuine
 *  engine crash, not a dropped connection.  fb-cpp changes nothing about
 *  that mechanism (embedded vs remote is the connection string, exactly as
 *  in the OO API); what changes is the ceremony around it: attach is one
 *  RAII object, the counts come back as std::optional<int64_t> from
 *  queryScalar, and there is no status vector in sight.
 *  See ../../careful-writes-and-crash-safety.md.
 *
 *  Run it against a scratch path only:
 *      ./build/fbcpp_careful_writes [/tmp/fbhandson/careful_writes_fbcpp.fdb]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <chrono>

using namespace fbcpp;
using namespace fbcpp_sample;
using clk = std::chrono::steady_clock;

static const char* DEFAULT_DB = "/tmp/fbhandson/careful_writes_fbcpp.fdb";

static long fileSize(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 ? long(st.st_size) : -1;
}

// Child mode: create the database, commit a marker, then bulk-insert half a
// million rows WITHOUT committing — and wait to be killed.
static int writer(const char* dbFile)
{
	Client client{"fbclient"};
	Attachment att = attachOrCreate(client, dbFile);

	Transaction ddl{att};
	att.execute(ddl, "create table cw (id int, tag varchar(30))");
	ddl.commit();

	Transaction marker{att};
	att.execute(marker, "insert into cw values (1, 'committed-marker')");
	marker.commit();
	printf("[writer %d] marker row committed (forced writes on)\n", getpid());
	fflush(stdout);

	Transaction victim{att};         // never committed: the crash victim
	att.execute(victim,
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
		Client client{"fbclient"};
		Attachment att{client, dbFile, defaultOptions()};
		Transaction t{att};
		const auto committed = att.queryScalar<std::int64_t>(t,
			"select count(*) from cw where tag = 'committed-marker'").value_or(-1);
		const auto uncommitted = att.queryScalar<std::int64_t>(t,
			"select count(*) from cw where tag = 'uncommitted'").value_or(-1);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			clk::now() - t0).count();
		t.commit();

		printf("re-attach + both counts took %lld ms\n", (long long) ms);
		printf("committed marker rows : %lld   <- survived the crash\n",
			(long long) committed);
		printf("uncommitted rows      : %lld   <- rolled back by visibility, not replay\n",
			(long long) uncommitted);
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
