/*
 *  parallel_workers.cpp (fb-cpp) — watching worker attachments appear (and be refused).
 *
 *  The fb-cpp twin of ../cpp/parallel_workers.cpp, same two phases:
 *
 *  [A] Against the live server: ask for 4 workers.  fb-cpp has no typed
 *      option for isc_dpb_parallel_workers, but AttachmentOptions::setDpb
 *      accepts raw DPB clumplets that the typed options are merged into —
 *      the escape hatch for exactly this case.  One real difference from
 *      the OO API: fb-cpp's status handling throws on errors but discards
 *      WARNINGS, so the isc_bad_par_workers warning that phase A of the
 *      OO sample fishes out of the status vector is invisible here.  The
 *      clamp is read back from SQL instead: MON$ATTACHMENTS.
 *      MON$PARALLEL_WORKERS shows what the engine actually granted.
 *
 *  [B] Against an embedded engine whose private FIREBIRD root sets
 *      ParallelWorkers = 4 / MaxParallelWorkers = 8 (the FIREBIRD env
 *      variable is read by the engine provider fbclient loads, so it
 *      works identically from fb-cpp): build an incompressible 200k-row
 *      table, CREATE INDEX, and poll MON$ATTACHMENTS from a second
 *      attachment while the build runs.  See ../../parallel-workers.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_parallel_workers    # ~30 s: builds a 40 MB scratch table
 */

#include "fbcpp_sample.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* ROOT = "/tmp/fbhandson/fbroot-parallel-fbcpp";

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

static std::string knobs(Attachment& att, Transaction& tra)
{
	auto knob = [&](const char* name) {
		return att.queryScalar<std::string>(tra,
			std::string("select rdb$config_value from rdb$config "
				"where rdb$config_name = '") + name + "'").value_or("?");
	};
	return "ParallelWorkers = " + knob("ParallelWorkers") +
		", MaxParallelWorkers = " + knob("MaxParallelWorkers");
}

int main(int argc, char** argv)
{
	setenv("FIREBIRD", ROOT, 1);	// read by the embedded engine of phase B
	makeRoot();

	const char* remoteDb = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/parallel_fbcpp.fdb");
	const char* embeddedDb = argOrDefault(argc, argv, 2,
		"/tmp/fbhandson/par_embedded_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};

		// --- [A] the server refuses politely ------------------------------
		{
			{ Attachment first = attachOrCreate(client, remoteDb); }	// ensure it exists

			// Raw DPB clumplets: isc_dpb_parallel_workers = 4 (typed options
			// are merged on top of this buffer).
			Attachment att{client, remoteDb, defaultOptions()
				.setDpb({isc_dpb_version1, isc_dpb_parallel_workers, 4, 4, 0, 0, 0})};

			printf("[A] server attach, isc_dpb_parallel_workers = 4 via raw DPB\n");
			Transaction tra{att};
			printf("    granted: mon$parallel_workers = %d "
				"(fb-cpp discards the isc_bad_par_workers warning)\n",
				att.queryScalar<std::int32_t>(tra,
					"select mon$parallel_workers from mon$attachments "
					"where mon$attachment_id = current_connection").value_or(-1));
			printf("    server config: %s -> request clamped, 0 extra workers\n\n",
				knobs(att, tra).c_str());
			tra.commit();
		}

		// --- [B] embedded engine with its own firebird.conf ---------------
		Attachment att = attachOrCreate(client, embeddedDb);
		Transaction tra{att};
		printf("[B] embedded attach, FIREBIRD=%s\n", ROOT);
		printf("    engine config: %s\n", knobs(att, tra).c_str());

		try { att.execute(tra, "drop table parade"); } catch (const DatabaseException&) {}
		att.execute(tra, "create table parade (id int, val varchar(200))");
		tra.commitRetaining();
		// The filler must be incompressible: records are RLE-compressed on
		// page, and IndexCreateTask::getMaxWorkers() goes parallel only if
		// the relation spans more than one pointer page.
		att.execute(tra,
			"execute block as declare n int = 0; begin "
			"  while (n < 200000) do begin "
			"    insert into parade values (:n, "
			"      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
			"      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || "
			"      uuid_to_char(gen_uuid())); "
			"    n = n + 1; "
			"  end end");
		tra.commitRetaining();
		printf("    parade table: 200000 rows of 180 incompressible bytes, %lld pointer pages\n",
			static_cast<long long>(att.queryScalar<std::int64_t>(tra,
				"select count(*) from rdb$pages p join rdb$relations r "
				"  on p.rdb$relation_id = r.rdb$relation_id "
				"where r.rdb$relation_name = 'PARADE' and p.rdb$page_type = 4").value_or(0)));

		std::atomic<int> maxSeen{0};
		std::atomic<bool> stop{false};
		std::string roster;
		std::thread poller([&] {
			try
			{
				// Second attachment, same process, same Client.
				Attachment mon{client, embeddedDb, defaultOptions()};
				while (!stop)
				{
					Transaction t{mon};		// fresh MON$ snapshot
					int n = static_cast<int>(*mon.queryScalar<std::int64_t>(t,
						"select count(*) from mon$attachments "
						"where mon$user = '<Worker>'"));
					if (n > maxSeen)
					{
						maxSeen = n;
						roster.clear();
						Statement all{mon, t,
							"select trim(mon$user), mon$system_flag "
							"from mon$attachments order by mon$attachment_id"};
						for (bool row = all.execute(t); row; row = all.fetchNext())
							roster += "        " + all.getString(0).value_or("?") +
								"  (system_flag " +
								std::to_string(all.getInt32(1).value_or(-1)) + ")\n";
					}
					t.commit();
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
			}
			catch (const FbCppException&) {}
		});

		auto t0 = std::chrono::steady_clock::now();
		att.execute(tra, "create index ix_parade on parade (val)");
		tra.commitRetaining();
		double ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
		stop = true;
		poller.join();

		printf("    create index: %.0f ms; max '<Worker>' attachments seen: %d\n",
			ms, int(maxSeen));
		printf("    MON$ATTACHMENTS at the widest moment:\n%s", roster.c_str());
		printf("    after build: workers stay pooled (idle timeout 60 s): %lld\n",
			static_cast<long long>(att.queryScalar<std::int64_t>(tra,
				"select count(*) from mon$attachments "
				"where mon$user = '<Worker>'").value_or(0)));
		tra.commit();
		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
