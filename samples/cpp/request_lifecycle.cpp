/*
 *  request_lifecycle.cpp — companion to ../../request-lifecycle-code-trace.md
 *
 *  One CREATE TABLE round trip — the document's exact scenario — instrumented
 *  from the client.  Each API step is timed, and around them the engine's own
 *  monitoring counters (MON$IO_STATS / MON$RECORD_STATS for this attachment)
 *  are sampled, so the stages of the trace become visible as numbers:
 *
 *    prepare              -> DSQL: parser picks DsqlDdlStatement (type DDL)
 *    execute              -> EXE/MET: STORE into RDB$RELATIONS etc. — the
 *                            record-insert counters jump (VIO_store), and the
 *                            new row is already visible to *this* transaction
 *    commit               -> TRA_commit -> DFW_perform_work -> CCH_flush ->
 *                            PIO_write — the page-write counter jumps
 *
 *  Uses its own scratch database — safe to re-run.
 */

#include "fb_sample.h"
#include <chrono>

using namespace Firebird;

struct Stats { long fetches, marks, writes, recIns; };

// Sample this attachment's cumulative counters in a fresh transaction
// (MON$ snapshots are frozen per transaction, so a new one sees fresh data).
static Stats sample(fbsample::Db& db)
{
	ITransaction* t = db.start();
	auto row = db.query(t,
		"SELECT i.MON$PAGE_FETCHES, i.MON$PAGE_MARKS, i.MON$PAGE_WRITES,"
		"       r.MON$RECORD_INSERTS"
		" FROM MON$ATTACHMENTS a"
		" JOIN MON$IO_STATS i ON a.MON$STAT_ID = i.MON$STAT_ID"
		" JOIN MON$RECORD_STATS r ON a.MON$STAT_ID = r.MON$STAT_ID"
		" WHERE a.MON$ATTACHMENT_ID = CURRENT_CONNECTION").rows.at(0);
	t->commit(&db.status);
	return { atol(row[0].c_str()), atol(row[1].c_str()),
	         atol(row[2].c_str()), atol(row[3].c_str()) };
}

static double since(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv)
{
	const char* database = fbsample::argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/request_lifecycle.fdb");

	try
	{
		fbsample::Db db;
		db.attachOrCreate(database);

		// Idempotency: drop a leftover table from a previous run, if any.
		try
		{
			ITransaction* t = db.start();
			db.exec(t, "DROP TABLE trace_demo");
			t->commit(&db.status);
		}
		catch (const FbException&) { db.status.init(); }

		const Stats s0 = sample(db);
		ITransaction* tra = db.start();

		// -- prepare: Y-valve -> remote -> DSQL (Stages 1-5) ---------------
		auto t0 = std::chrono::steady_clock::now();
		IStatement* stmt = db.att->prepare(&db.status, tra, 0,
			"CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY,"
			" name VARCHAR(30))", SQL_DIALECT_V6, 0);
		printf("prepare  %6.2f ms   statement type = %s\n", since(t0),
			stmt->getType(&db.status) == isc_info_sql_stmt_ddl ? "DDL" : "?");

		// -- execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) ----
		t0 = std::chrono::steady_clock::now();
		stmt->execute(&db.status, tra, nullptr, nullptr, nullptr, nullptr);
		const double tExec = since(t0);
		const Stats s1 = sample(db);
		printf("execute  %6.2f ms   catalog record inserts: +%ld, page marks: +%ld\n",
			tExec, s1.recIns - s0.recIns, s1.marks - s0.marks);

		// Uncommitted, but the STORE into RDB$RELATIONS is visible to the
		// transaction that did it:
		printf("         in this tx:  RDB$RELATIONS has TRACE_DEMO = %s\n",
			db.queryValue(tra, "SELECT COUNT(*) FROM RDB$RELATIONS"
				" WHERE RDB$RELATION_NAME = 'TRACE_DEMO'").c_str());

		// -- commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9) -
		t0 = std::chrono::steady_clock::now();
		tra->commit(&db.status);
		const double tCommit = since(t0);
		const Stats s2 = sample(db);
		printf("commit   %6.2f ms   page writes: +%ld  (fetches: +%ld over the whole trip)\n",
			tCommit, s2.writes - s1.writes, s2.fetches - s0.fetches);

		stmt->free(&db.status);
		printf("done.\n");
	}
	catch (const FbException& error)
	{
		return fbsample::report(error);
	}
	return 0;
}
