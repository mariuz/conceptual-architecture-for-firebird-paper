/*
 *  parallel_workers.cpp — watching worker attachments appear (and be refused).
 *
 *  Companion sample for ../../parallel-workers.md.  Two phases:
 *
 *  [A] Against the live server: ask for 4 workers via isc_dpb_parallel_workers.
 *      Both knobs are GLOBAL config (Config::getMaxParallelWorkers reads only
 *      the process's firebird.conf), so with the stock MaxParallelWorkers = 1
 *      the engine clamps the request and attaches with a WARNING — the
 *      "both knobs must be raised" lesson, delivered in the status vector.
 *
 *  [B] Against an embedded engine whose private FIREBIRD root sets
 *      ParallelWorkers = 4 / MaxParallelWorkers = 8: build a wide 200k-row
 *      table, CREATE INDEX on it, and poll MON$ATTACHMENTS from a second
 *      attachment while the build runs.  The workers appear as ordinary
 *      attachments — MON$USER = '<Worker>', MON$SYSTEM_FLAG = 1 — and stay
 *      pooled (idle timeout 60 s) after the build: parallelism built out of
 *      attachments, exactly as the document argues.
 */

#include "fb_sample.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace fbsample;

static const char* ROOT = "/tmp/fbhandson/fbroot-parallel";

// A private $FIREBIRD root: symlinks into the stock install, own firebird.conf.
static void makeRoot()
{
	mkdir(ROOT, 0775);
	for (const char* f : { "plugins", "intl", "firebird.msg", "tzdata",
		"plugins.conf", "databases.conf" })
	{
		std::string t = std::string("/opt/firebird/") + f;
		symlink(t.c_str(), (std::string(ROOT) + "/" + f).c_str());
	}
	FILE* conf = fopen((std::string(ROOT) + "/firebird.conf").c_str(), "w");
	fputs("ServerMode = Super\nParallelWorkers = 4\nMaxParallelWorkers = 8\n", conf);
	fclose(conf);
}

static std::string knobs(Db& db, ITransaction* tra)
{
	return "ParallelWorkers = " + db.queryValue(tra,
			"select rdb$config_value from rdb$config "
			"where rdb$config_name = 'ParallelWorkers'") +
		", MaxParallelWorkers = " + db.queryValue(tra,
			"select rdb$config_value from rdb$config "
			"where rdb$config_name = 'MaxParallelWorkers'");
}

int main(int argc, char** argv)
{
	setenv("FIREBIRD", ROOT, 1);	// read by the embedded engine of phase B
	makeRoot();

	const char* remoteDb = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/parallel.fdb");
	const char* embeddedDb = argOrDefault(argc, argv, 2,
		"/tmp/fbhandson/par_embedded.fdb");

	try
	{
		// --- [A] the server refuses politely ------------------------------
		{
			Db db;
			{ Db first; first.attachOrCreate(remoteDb); }	// ensure it exists

			IXpbBuilder* dpb = db.makeDpb(Db::defaultUser(), Db::defaultPassword(), "UTF8");
			dpb->insertInt(&db.status, isc_dpb_parallel_workers, 4);
			db.att = db.prov->attachDatabase(&db.status, remoteDb,
				dpb->getBufferLength(&db.status), dpb->getBuffer(&db.status));
			dpb->dispose();

			printf("[A] server attach, isc_dpb_parallel_workers = 4\n");
			const intptr_t* w = db.status.getWarnings();
			if (w[0] == isc_arg_warning && w[1] == isc_bad_par_workers)
				printf("    warning isc_bad_par_workers: wrong parallel workers "
					"value %ld, valid range are from 1 to %ld\n",
					long(w[3]), long(w[5]));
			ITransaction* tra = db.start();
			printf("    server config: %s -> request clamped, 0 extra workers\n\n",
				knobs(db, tra).c_str());
			tra->commit(&db.status);
		}

		// --- [B] embedded engine with its own firebird.conf ---------------
		Db db;
		db.attachOrCreate(embeddedDb);
		ITransaction* tra = db.start();
		printf("[B] embedded attach, FIREBIRD=%s\n", ROOT);
		printf("    engine config: %s\n", knobs(db, tra).c_str());

		try { db.exec(tra, "drop table parade"); } catch (const FbException&) {}
		db.exec(tra, "create table parade (id int, val varchar(200))");
		tra->commitRetaining(&db.status);
		// The filler must be incompressible: records are RLE-compressed on
		// page, and IndexCreateTask::getMaxWorkers() goes parallel only if
		// the relation spans more than one pointer page.
		db.exec(tra,
			"execute block as declare n int = 0; begin "
			"  while (n < 200000) do begin "
			"    insert into parade values (:n, "
			"      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
			"      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
			"      uuid_to_char(gen_uuid())); "
			"    n = n + 1; "
			"  end end");
		tra->commitRetaining(&db.status);
		printf("    parade table: 200000 rows of 180 incompressible bytes, %s pointer pages\n",
			db.queryValue(tra,
				"select count(*) from rdb$pages p join rdb$relations r "
				"  on p.rdb$relation_id = r.rdb$relation_id "
				"where r.rdb$relation_name = 'PARADE' and p.rdb$page_type = 4").c_str());

		std::atomic<int> maxSeen{0};
		std::atomic<bool> stop{false};
		std::string roster;
		std::thread poller([&] {
			try
			{
				Db mon;				// second attachment, same process
				mon.attach(embeddedDb);
				while (!stop)
				{
					ITransaction* t = mon.start();	// fresh MON$ snapshot
					int n = atoi(mon.queryValue(t,
						"select count(*) from mon$attachments "
						"where mon$user = '<Worker>'").c_str());
					if (n > maxSeen)
					{
						maxSeen = n;
						roster.clear();
						for (auto& r : mon.query(t,
							"select trim(mon$user), mon$system_flag "
							"from mon$attachments order by mon$attachment_id").rows)
							roster += "        " + r[0] + "  (system_flag " + r[1] + ")\n";
					}
					t->commit(&mon.status);
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
			}
			catch (const FbException&) {}
		});

		auto t0 = std::chrono::steady_clock::now();
		db.exec(tra, "create index ix_parade on parade (val)");
		tra->commitRetaining(&db.status);
		double ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
		stop = true;
		poller.join();

		printf("    create index: %.0f ms; max '<Worker>' attachments seen: %d\n",
			ms, int(maxSeen));
		printf("    MON$ATTACHMENTS at the widest moment:\n%s", roster.c_str());
		printf("    after build: workers stay pooled (idle timeout 60 s): %s\n",
			db.queryValue(tra,
				"select count(*) from mon$attachments "
				"where mon$user = '<Worker>'").c_str());
		tra->commit(&db.status);
		printf("done.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
