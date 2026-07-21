/*
 *  lock_manager.cpp (fb-cpp) — the three wait outcomes, plus a deadlock.
 *
 *  The fb-cpp twin of ../cpp/lock_manager.cpp.  The OO-API version reaches
 *  `SET TRANSACTION ... RESERVING` through the raw IAttachment::execute
 *  trick (execute with no current transaction returns the new one); fb-cpp
 *  has that exact idiom as a first-class constructor —
 *  Transaction{attachment, "SET TRANSACTION ..."} — so the table-reservation
 *  probes read like ordinary object construction that either succeeds
 *  (granted) or throws a DatabaseException (NO WAIT / LOCK TIMEOUT).  The
 *  deadlock act uses default-constructed transactions (empty TPB -> engine
 *  default SNAPSHOT WAIT), same as the OO-API sample.
 *  See ../../lock-manager.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_lock_manager [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace fbcpp;
using namespace fbcpp_sample;
using clk = std::chrono::steady_clock;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/lock_manager_fbcpp.fdb";

static double secondsSince(clk::time_point t0)
{
	return std::chrono::duration<double>(clk::now() - t0).count();
}

static std::string firstLine(const char* what)
{
	std::string s{what};
	return s.substr(0, s.find('\n'));
}

static void probe(Attachment& att, const char* label, const char* sql)
{
	auto t0 = clk::now();
	try
	{
		Transaction t{att, sql};
		printf("%-16s granted after %.3f s\n", label, secondsSince(t0));
		t.commit();
	}
	catch (const DatabaseException& e)
	{
		printf("%-16s failed after %.3f s: %s\n", label, secondsSince(t0),
			firstLine(e.what()).c_str());
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Client client{"fbclient"};
		Attachment a = attachOrCreate(client, database);
		Attachment b{client, database, defaultOptions()};

		{
			Transaction ddl{a};
			try { a.execute(ddl, "drop table t1"); }
			catch (const DatabaseException&) {}
			ddl.commit();
			Transaction ddl2{a};
			a.execute(ddl2, "create table t1 (id int primary key, v int)");
			ddl2.commit();
			Transaction ins{a};
			a.execute(ins, "insert into t1 values (1, 0)");
			a.execute(ins, "insert into t1 values (2, 0)");
			ins.commit();
		}

		// A holds the LCK_relation lock at EX for the whole first act.
		Transaction hold{a, "SET TRANSACTION WAIT RESERVING t1 FOR PROTECTED WRITE"};
		printf("holder: t1 reserved FOR PROTECTED WRITE (LCK_relation at LCK_EX)\n");

		probe(b, "NO WAIT:", "SET TRANSACTION NO WAIT RESERVING t1 FOR PROTECTED WRITE");
		probe(b, "LOCK TIMEOUT 3:", "SET TRANSACTION LOCK TIMEOUT 3 RESERVING t1 FOR PROTECTED WRITE");

		// WAIT parks in wait_for_request until the holder lets go: release
		// the reservation from another thread after 2 s.
		std::thread releaser([&]() {
			std::this_thread::sleep_for(std::chrono::seconds(2));
			hold.commit();
			printf("holder: committed (2 s later) -> lock released\n");
		});
		probe(b, "WAIT:", "SET TRANSACTION WAIT RESERVING t1 FOR PROTECTED WRITE");
		releaser.join();

		// Act two: a genuine wait-for cycle through LCK_tra locks.  Both
		// sides block in WAIT mode; nobody looks for the cycle until the
		// periodic scan fires — expect ~DeadlockTimeout seconds, not ~0.
		printf("building deadlock: A updates row 1, B updates row 2, then cross...\n");
		Transaction ta{a};                  // engine default: SNAPSHOT WAIT
		Transaction tb{b};
		a.execute(ta, "update t1 set v = v + 1 where id = 1");
		b.execute(tb, "update t1 set v = v + 1 where id = 2");

		std::atomic<bool> aVictim{false}, bVictim{false};
		auto t0 = clk::now();
		std::thread crossA([&]() {
			try { a.execute(ta, "update t1 set v = v + 1 where id = 2"); }
			catch (const DatabaseException& e)  // this side was chosen as victim
			{
				printf("deadlock: A failed after %.1f s: %s\n", secondsSince(t0),
					firstLine(e.what()).c_str());
				aVictim = true;
				ta.rollback();              // free B
			}
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		try
		{
			b.execute(tb, "update t1 set v = v + 1 where id = 1");
			printf("deadlock: B's update proceeded after %.1f s (A was the victim)\n",
				secondsSince(t0));
		}
		catch (const DatabaseException& e)
		{
			printf("deadlock: B failed after %.1f s: %s\n", secondsSince(t0),
				firstLine(e.what()).c_str());
			bVictim = true;
			tb.rollback();                  // free A
		}
		crossA.join();
		if (!aVictim) ta.rollback();
		if (!bVictim) tb.rollback();
		printf("the wait is DeadlockTimeout (10 s default): the cycle sat "
			"undetected until the scan.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
