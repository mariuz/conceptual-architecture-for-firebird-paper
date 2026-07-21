/*
 *  gc_sweep.cpp (fb-cpp) — the record-version lifecycle in MON$ counters.
 *
 *  The fb-cpp twin of ../cpp/gc_sweep.cpp: pin a SNAPSHOT, commit twelve
 *  updates under it, release, scan, delete, then freeze the OIT with a
 *  no_auto_undo rollback.  The wrapper's aggregate binding does the MON$
 *  bookkeeping: each counter row is fetched straight into a plain struct
 *  via queryFirstRowAs<T>(), and the two TPB tricks of the OO-API version
 *  (isc_tpb_concurrency, isc_tpb_no_auto_undo) become
 *  setIsolationLevel(SNAPSHOT) and setNoAutoUndo(true).
 *  See ../../garbage-collection-and-sweep.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_gc_sweep [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <thread>
#include <chrono>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/gc_sweep_fbcpp.fdb";

// One row of MON$RECORD_STATS / MON$DATABASE, mapped by position onto a
// struct — fb-cpp checks the field count against the column count.
struct RecordStats
{
	std::int64_t upd, imgc, purges, expunges, backreads;
};

struct HeaderCounters
{
	std::int64_t oit, oat, ost, next, sweepInterval;
};

// MON$ tables are a stable snapshot per transaction: use a fresh transaction
// for every peek so the counters are current.
static void showStats(Attachment& att, const char* label)
{
	Transaction t{att};
	const RecordStats s = att.queryFirstRowAs<RecordStats>(t,
		"select r.MON$RECORD_UPDATES, r.MON$RECORD_IMGC, "
		"       r.MON$RECORD_PURGES, r.MON$RECORD_EXPUNGES, "
		"       r.MON$BACKVERSION_READS "
		"from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)").value();
	printf("%-34s upd=%-4lld imgc=%-3lld purges=%-3lld expunges=%-3lld backreads=%lld\n",
		label, (long long) s.upd, (long long) s.imgc, (long long) s.purges,
		(long long) s.expunges, (long long) s.backreads);
	t.commit();
}

static void showCounters(Attachment& att, const char* label)
{
	Transaction t{att};
	const HeaderCounters c = att.queryFirstRowAs<HeaderCounters>(t,
		"select MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$OLDEST_SNAPSHOT, "
		"       MON$NEXT_TRANSACTION, MON$SWEEP_INTERVAL from MON$DATABASE").value();
	printf("%-34s OIT=%lld OAT=%lld OST=%lld Next=%lld (sweep interval %lld)\n",
		label, (long long) c.oit, (long long) c.oat, (long long) c.ost,
		(long long) c.next, (long long) c.sweepInterval);
	t.commit();
}

static int readVal(Attachment& att, Transaction& t)
{
	return att.queryScalar<std::int32_t>(t,
		"select val from gctest where id = 1").value_or(-1);
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Client client{"fbclient"};
		Attachment writer = attachOrCreate(client, database);
		Attachment pinner{client, database, defaultOptions()};

		{
			Transaction ddl{writer};
			try { writer.execute(ddl, "drop table gctest"); }
			catch (const DatabaseException&) {}
			ddl.commit();
			Transaction ddl2{writer};
			writer.execute(ddl2, "create table gctest (id int primary key, val int)");
			ddl2.commit();
			Transaction ins{writer};
			writer.execute(ins, "insert into gctest values (1, 0)");
			ins.commit();
		}

		// 1. Pin a snapshot: while this SNAPSHOT transaction lives, its
		//    tra_oldest_active holds the OST down and version 0 must survive.
		Transaction snap{pinner, TransactionOptions()
			.setIsolationLevel(TransactionIsolationLevel::SNAPSHOT)};
		printf("pinned SNAPSHOT reads val = %d\n", readVal(pinner, snap));
		showStats(writer, "before updates:");

		// 2. Twelve committed updates -> twelve back versions... in theory.
		//    One prepared statement, re-executed under a fresh transaction
		//    each time with a typed parameter.
		{
			Transaction prep{writer};
			Statement upd{writer, prep, "update gctest set val = ? where id = 1"};
			prep.commit();
			for (std::int32_t i = 1; i <= 12; ++i)
			{
				Transaction t{writer};
				upd.set(0, i);
				upd.execute(t);
				t.commit();
			}
		}
		showStats(writer, "after 12 updates (snapshot open):");
		printf("pinned SNAPSHOT still reads val = %d\n", readVal(pinner, snap));

		// 3. Release the snapshot; a sequential scan now trips over the
		//    below-OST chain (cooperative GC) and/or notifies the GC thread.
		snap.commit();
		{
			Transaction t{writer};
			printf("snapshot released; new reader sees val = %d\n", readVal(writer, t));
			t.commit();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		showStats(writer, "after release + scan + 1.5s:");

		// 4. A committed DELETE older than the OST is expunged, not purged.
		{
			Transaction t{writer};
			writer.execute(t, "delete from gctest where id = 1");
			t.commit();
			Transaction t2{writer};
			writer.queryScalar<std::int64_t>(t2, "select count(*) from gctest");
			t2.commit();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		showStats(writer, "after DELETE + scan + 1.5s:");

		// 5. A rolled-back transaction becomes an "interesting" stump: with
		//    no_auto_undo the rollback is recorded only in the TIP, so the
		//    OIT freezes there until a sweep rewrites its state.
		showCounters(writer, "header counters before rollback:");
		{
			Transaction t{writer, TransactionOptions()
				.setIsolationLevel(TransactionIsolationLevel::SNAPSHOT)
				.setNoAutoUndo(true)};
			writer.execute(t, "insert into gctest values (2, 0)");
			t.rollback();
		}
		showCounters(writer, "after no_auto_undo rollback:");
		printf("run 'gfix -sweep' (or wait for OAT-OIT > interval) "
			"to move the OIT past the stump.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
