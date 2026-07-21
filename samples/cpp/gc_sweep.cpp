/*
 *  gc_sweep.cpp — companion sample for garbage-collection-and-sweep.md.
 *
 *  Creates record versions under a pinned SNAPSHOT, then releases the
 *  snapshot and watches the collectors work — entirely through the
 *  database-level MON$RECORD_STATS counters, which count exactly the
 *  vio.cpp events the document describes:
 *
 *      MON$RECORD_IMGC     — VIO_intermediate_gc collections (FB5+)
 *      MON$RECORD_PURGES   — purge():   chain trimmed, record lives on
 *      MON$RECORD_EXPUNGES — expunge(): committed delete fully removed
 *      MON$BACKVERSION_READS — version-chain walks readers had to do
 *
 *  Also prints the four header counters (OIT / OAT / OST / Next) around a
 *  rollback, showing a rolled-back stump pinning the OIT until sweep.
 */

#include "fb_sample.h"
#include <thread>
#include <chrono>

using namespace fbsample;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/gc_sweep.fdb";

// MON$ tables are a stable snapshot per transaction: use a fresh transaction
// for every peek so the counters are current.
static void showStats(Db& db, const char* label)
{
	ITransaction* t = db.start();
	Db::Table s = db.query(t,
		"select r.MON$RECORD_UPDATES upd, r.MON$RECORD_IMGC imgc, "
		"       r.MON$RECORD_PURGES purges, r.MON$RECORD_EXPUNGES expunges, "
		"       r.MON$BACKVERSION_READS backreads "
		"from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)");
	printf("%-34s upd=%-4s imgc=%-3s purges=%-3s expunges=%-3s backreads=%s\n",
		label, s.rows[0][0].c_str(), s.rows[0][1].c_str(),
		s.rows[0][2].c_str(), s.rows[0][3].c_str(), s.rows[0][4].c_str());
	t->commit(&db.status);
}

static void showCounters(Db& db, const char* label)
{
	ITransaction* t = db.start();
	Db::Table c = db.query(t,
		"select MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$OLDEST_SNAPSHOT, "
		"       MON$NEXT_TRANSACTION, MON$SWEEP_INTERVAL from MON$DATABASE");
	printf("%-34s OIT=%s OAT=%s OST=%s Next=%s (sweep interval %s)\n",
		label, c.rows[0][0].c_str(), c.rows[0][1].c_str(),
		c.rows[0][2].c_str(), c.rows[0][3].c_str(), c.rows[0][4].c_str());
	t->commit(&db.status);
}

int main(int argc, char** argv)
{
	const char* dbName = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Db writer, pinner;
		writer.attachOrCreate(dbName);
		pinner.attach(dbName);

		ITransaction* ddl = writer.start();
		try { writer.exec(ddl, "drop table gctest"); } catch (const FbException&) {}
		ddl->commit(&writer.status);
		ddl = writer.start();
		writer.exec(ddl, "create table gctest (id int primary key, val int)");
		ddl->commit(&writer.status);
		ddl = writer.start();
		writer.exec(ddl, "insert into gctest values (1, 0)");
		ddl->commit(&writer.status);

		// 1. Pin a snapshot: while this SNAPSHOT transaction lives, its
		//    tra_oldest_active holds the OST down and version 0 must survive.
		ITransaction* snap = pinner.start(tpb({isc_tpb_concurrency}));
		printf("pinned SNAPSHOT reads val = %s\n",
			pinner.queryValue(snap, "select val from gctest where id = 1").c_str());
		showStats(writer, "before updates:");

		// 2. Twelve committed updates -> twelve back versions... in theory.
		for (int i = 1; i <= 12; ++i)
		{
			ITransaction* t = writer.start();
			writer.exec(t, "update gctest set val = " + std::to_string(i)
				+ " where id = 1");
			t->commit(&writer.status);
		}
		showStats(writer, "after 12 updates (snapshot open):");
		printf("pinned SNAPSHOT still reads val = %s\n",
			pinner.queryValue(snap, "select val from gctest where id = 1").c_str());

		// 3. Release the snapshot; a sequential scan now trips over the
		//    below-OST chain (cooperative GC) and/or notifies the GC thread.
		snap->commit(&pinner.status);
		{
			ITransaction* t = writer.start();
			printf("snapshot released; new reader sees val = %s\n",
				writer.queryValue(t, "select val from gctest where id = 1").c_str());
			t->commit(&writer.status);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		showStats(writer, "after release + scan + 1.5s:");

		// 4. A committed DELETE older than the OST is expunged, not purged.
		ITransaction* t = writer.start();
		writer.exec(t, "delete from gctest where id = 1");
		t->commit(&writer.status);
		t = writer.start();
		writer.queryValue(t, "select count(*) from gctest");   // scan -> collect
		t->commit(&writer.status);
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		showStats(writer, "after DELETE + scan + 1.5s:");

		// 5. A rolled-back transaction becomes an "interesting" stump: with
		//    no_auto_undo the rollback is recorded only in the TIP, so the
		//    OIT freezes there until a sweep rewrites its state.
		showCounters(writer, "header counters before rollback:");
		t = writer.start(tpb({isc_tpb_concurrency, isc_tpb_no_auto_undo}));
		writer.exec(t, "insert into gctest values (2, 0)");
		t->rollback(&writer.status);
		showCounters(writer, "after no_auto_undo rollback:");
		printf("run 'gfix -sweep' (or wait for OAT-OIT > interval) "
			"to move the OIT past the stump.\n");
		return 0;
	}
	catch (const FbException& e) { return report(e); }
}
