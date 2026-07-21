/*
 *  pooling.cpp (fb-cpp) — both directions of pooling from one C++ program.
 *
 *  The first half is the fb-cpp twin of ../cpp/pooling.cpp: the server's
 *  OUTBOUND (EDS) pool tuned and watched through the same SQL statements and
 *  context variables.  The second half is what the OO-API sample cannot
 *  show: fb-cpp ships an INBOUND, client-side pool — AttachmentPool, with
 *  PooledAttachment as an RAII lease — i.e. the driver-level pooling the
 *  companion document names as Firebird's inbound story, here in C++ rather
 *  than in a driver.  See ../../connection-pooling.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_pooling [database] [external-dsn]
 */

#include "fbcpp_sample.h"
#include <chrono>
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

// The EDS pool's four SYSTEM context variables in one row.
static void poolState(Attachment& att, Transaction& tra, const char* moment)
{
	Statement s{att, tra,
		"select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_SIZE'),"
		"       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_LIFETIME'),"
		"       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT'),"
		"       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT')"
		" from rdb$database"};
	s.execute(tra);
	printf("%-18s size=%s lifetime=%ss idle=%s active=%s\n", moment,
		s.getString(0).value_or("?").c_str(), s.getString(1).value_or("?").c_str(),
		s.getString(2).value_or("?").c_str(), s.getString(3).value_or("?").c_str());
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/pooling_fbcpp.fdb");
	const char* external = argOrDefault(argc, argv, 2, database);

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		// --- 1. OUTBOUND: the server's EDS pool, as in the OO-API sample ----
		Transaction tra{att};
		att.execute(tra, "alter external connections pool set size 5");
		att.execute(tra, "alter external connections pool set lifetime 30 second");
		tra.commitRetaining();

		poolState(att, tra, "before:");

		// Three EXECUTE STATEMENT ON EXTERNAL calls to the same
		// (connection string, user, password, role) — the pool's key.
		std::string block =
			"execute block returns (idle varchar(10), active varchar(10)) as\n"
			"  declare i int = 0;\n"
			"  declare v int;\n"
			"begin\n"
			"  while (i < 3) do\n"
			"  begin\n"
			"    execute statement 'select 1 from rdb$database'\n"
			"      on external '" + std::string(external) + "'\n"
			"      as user '" + envOr("ISC_USER", "SYSDBA") + "'"
			" password '" + envOr("ISC_PASSWORD", "masterkey") + "'\n"
			"      into :v;\n"
			"    i = i + 1;\n"
			"  end\n"
			"  idle   = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');\n"
			"  active = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');\n"
			"  suspend;\n"
			"end";
		Statement in{att, tra, block};
		in.execute(tra);
		printf("%-18s idle=%s active=%s   (3 calls, 1 outbound connection)\n",
			"inside the block:", in.getString(0).value_or("?").c_str(),
			in.getString(1).value_or("?").c_str());

		// Only a FULL commit releases the external connection to the idle
		// list (COMMIT RETAINING keeps it active — see the OO-API sample).
		tra.commit();
		Transaction tra2{att};
		poolState(att, tra2, "after commit:");

		att.execute(tra2, "alter external connections pool clear all");
		poolState(att, tra2, "after CLEAR ALL:");
		tra2.commit();

		// --- 2. INBOUND: fb-cpp's client-side AttachmentPool ----------------
		// What EDS does for the server's outbound connections, AttachmentPool
		// does for OUR attachments: max size, acquire timeout, and ALTER
		// SESSION RESET on release — the same reset EDS runs before parking.
		AttachmentPool pool{client, database, AttachmentPoolOptions()
			.setAttachmentOptions(defaultOptions())
			.setMaxSize(2)
			.setAcquireTimeout(std::chrono::milliseconds(300))
			.setSessionResetOnRelease(true)};

		PooledAttachment a = pool.acquire();
		PooledAttachment b = pool.acquire();
		printf("\ntook 2 of max 2:   total=%zu available=%zu inUse=%zu\n",
			pool.size(), pool.availableCount(), pool.inUseCount());

		auto t0 = std::chrono::steady_clock::now();
		auto third = pool.tryAcquire();          // pool exhausted: waits, then gives up
		auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		printf("asked for a 3rd:   %s after %lld ms (pool exhausted)\n",
			third ? "served (UNEXPECTED)" : "timed out", (long long) waited);

		b.release();                             // the RAII lease goes back...
		third = pool.tryAcquire();               // ...and the 3rd ask is served
		Transaction t{third->get()};
		printf("released one:      3rd acquire served, CURRENT_CONNECTION = %lld\n",
			(long long) third->get().queryScalar<std::int64_t>(t,
				"select current_connection from rdb$database").value_or(-1));
		t.commit();
		third->release();
		a.release();
		pool.close();                            // now the real detaches happen

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
