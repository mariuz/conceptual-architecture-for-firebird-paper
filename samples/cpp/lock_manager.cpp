/*
 *  lock_manager.cpp — companion sample for lock-manager.md.
 *
 *  Drives the real lock manager from SQL and times all three wait outcomes.
 *  `SET TRANSACTION ... RESERVING t1 FOR PROTECTED WRITE` takes a genuine
 *  LCK_relation lock at LCK_EX (tra.cpp maps lock_write+protected to EX),
 *  so the probes below exercise enqueue/grant_or_que/wait_for_request —
 *  not the MVCC record-conflict path:
 *
 *      NO WAIT        -> isc_lock_conflict, immediately   (lck_wait == 0)
 *      LOCK TIMEOUT 3 -> isc_lock_timeout, after ~3 s     (lck_wait < 0)
 *      WAIT           -> granted the moment the holder commits (lck_wait > 0)
 *
 *  A final act builds a real deadlock through LCK_tra transaction locks and
 *  measures how long the periodic scanner (DeadlockTimeout = 10 s) takes to
 *  find it.
 */

#include "fb_sample.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace fbsample;
using clk = std::chrono::steady_clock;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/lock_manager.fdb";

static double secondsSince(clk::time_point t0)
{
	return std::chrono::duration<double>(clk::now() - t0).count();
}

// Run "SET TRANSACTION ..." — executing it with no current transaction
// returns the newly started ITransaction.
static ITransaction* setTransaction(Db& db, const std::string& sql)
{
	return db.att->execute(&db.status, nullptr, 0, sql.c_str(), SQL_DIALECT_V6,
		nullptr, nullptr, nullptr, nullptr);
}

static void probe(Db& db, const char* label, const std::string& sql)
{
	auto t0 = clk::now();
	try
	{
		ITransaction* t = setTransaction(db, sql);
		printf("%-16s granted after %.3f s\n", label, secondsSince(t0));
		t->commit(&db.status);
	}
	catch (const FbException& e)
	{
		char buf[256];
		master->getUtilInterface()->formatStatus(buf, sizeof(buf), e.getStatus());
		*strchrnul(buf, '\n') = 0;              // first line only
		printf("%-16s failed after %.3f s: %s\n", label, secondsSince(t0), buf);
	}
}

int main(int argc, char** argv)
{
	const char* dbName = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Db a, b;
		a.attachOrCreate(dbName);
		b.attach(dbName);

		ITransaction* ddl = a.start();
		try { a.exec(ddl, "drop table t1"); } catch (const FbException&) {}
		ddl->commit(&a.status);
		ddl = a.start();
		a.exec(ddl, "create table t1 (id int primary key, v int)");
		ddl->commit(&a.status);
		ddl = a.start();
		a.exec(ddl, "insert into t1 values (1, 0)");
		a.exec(ddl, "insert into t1 values (2, 0)");
		ddl->commit(&a.status);

		// A holds the LCK_relation lock at EX for the whole first act.
		ITransaction* hold = setTransaction(a,
			"SET TRANSACTION WAIT RESERVING t1 FOR PROTECTED WRITE");
		printf("holder: t1 reserved FOR PROTECTED WRITE (LCK_relation at LCK_EX)\n");

		probe(b, "NO WAIT:", "SET TRANSACTION NO WAIT RESERVING t1 FOR PROTECTED WRITE");
		probe(b, "LOCK TIMEOUT 3:", "SET TRANSACTION LOCK TIMEOUT 3 RESERVING t1 FOR PROTECTED WRITE");

		// WAIT parks in wait_for_request until the holder lets go: release
		// the reservation from another thread after 2 s.
		std::thread releaser([&]() {
			std::this_thread::sleep_for(std::chrono::seconds(2));
			CheckStatusWrapper st(master->getStatus());
			hold->commit(&st);
			st.dispose();
			printf("holder: committed (2 s later) -> lock released\n");
		});
		probe(b, "WAIT:", "SET TRANSACTION WAIT RESERVING t1 FOR PROTECTED WRITE");
		releaser.join();

		// Act two: a genuine wait-for cycle through LCK_tra locks.  Both
		// sides block in WAIT mode; nobody looks for the cycle until the
		// periodic scan fires — expect ~DeadlockTimeout seconds, not ~0.
		printf("building deadlock: A updates row 1, B updates row 2, then cross...\n");
		ITransaction* ta = a.start();       // engine default: SNAPSHOT WAIT
		ITransaction* tb = b.start();
		a.exec(ta, "update t1 set v = v + 1 where id = 1");
		b.exec(tb, "update t1 set v = v + 1 where id = 2");

		std::atomic<bool> aVictim{false}, bVictim{false};
		auto t0 = clk::now();
		std::thread crossA([&]() {
			try { a.exec(ta, "update t1 set v = v + 1 where id = 2"); }
			catch (const FbException& e)    // this side was chosen as victim
			{
				char buf[256];
				master->getUtilInterface()->formatStatus(buf, sizeof(buf), e.getStatus());
				*strchrnul(buf, '\n') = 0;
				printf("deadlock: A failed after %.1f s: %s\n", secondsSince(t0), buf);
				aVictim = true;
				ta->rollback(&a.status);    // free B
			}
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		try
		{
			b.exec(tb, "update t1 set v = v + 1 where id = 1");
			printf("deadlock: B's update proceeded after %.1f s (A was the victim)\n",
				secondsSince(t0));
		}
		catch (const FbException& e)
		{
			char buf[256];
			master->getUtilInterface()->formatStatus(buf, sizeof(buf), e.getStatus());
			*strchrnul(buf, '\n') = 0;
			printf("deadlock: B failed after %.1f s: %s\n", secondsSince(t0), buf);
			bVictim = true;
			tb->rollback(&b.status);        // free A
		}
		crossA.join();
		if (!aVictim) ta->rollback(&a.status);
		if (!bVictim) tb->rollback(&b.status);
		printf("the wait is DeadlockTimeout (10 s default): the cycle sat "
			"undetected until the scan.\n");
		return 0;
	}
	catch (const FbException& e) { return report(e); }
}
