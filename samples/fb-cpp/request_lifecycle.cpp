/*
 *  request_lifecycle.cpp (fb-cpp) — one CREATE TABLE round trip, timed.
 *
 *  The fb-cpp twin of ../cpp/request_lifecycle.cpp: the document's exact
 *  scenario — prepare, execute and commit of one CREATE TABLE — with the
 *  attachment's MON$IO_STATS/MON$RECORD_STATS counters sampled around each
 *  step.  The three stages map onto fb-cpp's RAII surface directly:
 *  *prepare* is the Statement constructor (getType() returns the typed
 *  StatementType::DDL instead of an info-buffer constant), *execute* is
 *  stmt.execute(tra), *commit* is tra.commit(); the MON$ sampling query
 *  reads its BIGINT counters through std::optional<std::int64_t>.
 *  See ../../request-lifecycle-code-trace.md.
 *
 *  Uses its own scratch database — safe to re-run.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_request_lifecycle [database]
 */

#include "fbcpp_sample.h"
#include <chrono>
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

struct Stats { long fetches, marks, writes, recIns; };

// Sample this attachment's cumulative counters in a fresh transaction
// (MON$ snapshots are frozen per transaction, so a new one sees fresh data).
static Stats sample(Attachment& att)
{
	Transaction t{att};
	Statement stmt{att, t,
		"SELECT i.MON$PAGE_FETCHES, i.MON$PAGE_MARKS, i.MON$PAGE_WRITES,"
		"       r.MON$RECORD_INSERTS"
		" FROM MON$ATTACHMENTS a"
		" JOIN MON$IO_STATS i ON a.MON$STAT_ID = i.MON$STAT_ID"
		" JOIN MON$RECORD_STATS r ON a.MON$STAT_ID = r.MON$STAT_ID"
		" WHERE a.MON$ATTACHMENT_ID = CURRENT_CONNECTION"};
	stmt.execute(t);
	const Stats s = {
		static_cast<long>(stmt.getInt64(0).value_or(0)),
		static_cast<long>(stmt.getInt64(1).value_or(0)),
		static_cast<long>(stmt.getInt64(2).value_or(0)),
		static_cast<long>(stmt.getInt64(3).value_or(0)),
	};
	t.commit();
	return s;
}

static double since(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/request_lifecycle_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		// Idempotency: drop a leftover table from a previous run, if any.
		try
		{
			Transaction t{att};
			Statement{att, t, "DROP TABLE trace_demo"}.execute(t);
			t.commit();
		}
		catch (const DatabaseException&) {}

		const Stats s0 = sample(att);
		Transaction tra{att};

		// -- prepare: Y-valve -> remote -> DSQL (Stages 1-5) ---------------
		auto t0 = std::chrono::steady_clock::now();
		Statement stmt{att, tra,
			"CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY,"
			" name VARCHAR(30))"};
		printf("prepare  %6.2f ms   statement type = %s\n", since(t0),
			stmt.getType() == StatementType::DDL ? "DDL" : "?");

		// -- execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) ----
		t0 = std::chrono::steady_clock::now();
		stmt.execute(tra);
		const double tExec = since(t0);
		const Stats s1 = sample(att);
		printf("execute  %6.2f ms   catalog record inserts: +%ld, page marks: +%ld\n",
			tExec, s1.recIns - s0.recIns, s1.marks - s0.marks);

		// Uncommitted, but the STORE into RDB$RELATIONS is visible to the
		// transaction that did it:
		Statement count{att, tra, "SELECT COUNT(*) FROM RDB$RELATIONS"
			" WHERE RDB$RELATION_NAME = 'TRACE_DEMO'"};
		count.execute(tra);
		printf("         in this tx:  RDB$RELATIONS has TRACE_DEMO = %ld\n",
			static_cast<long>(count.getInt64(0).value_or(0)));

		// -- commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9) -
		t0 = std::chrono::steady_clock::now();
		tra.commit();
		const double tCommit = since(t0);
		const Stats s2 = sample(att);
		printf("commit   %6.2f ms   page writes: +%ld  (fetches: +%ld over the whole trip)\n",
			tCommit, s2.writes - s1.writes, s2.fetches - s0.fetches);

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
