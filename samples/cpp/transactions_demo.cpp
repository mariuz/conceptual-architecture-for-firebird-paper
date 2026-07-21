/*
 *  transactions_demo.cpp — MVCC isolation seen from the client.
 *
 *  Companion sample for ../../transactions-and-concurrency.md.  Two
 *  attachments to the same database play the paper's concurrency scenario:
 *
 *    1. SNAPSHOT stability — a reader holding a SNAPSHOT transaction keeps
 *       seeing the version that was committed at its start, while a second
 *       attachment commits a change "underneath" it.
 *    2. READ COMMITTED — the same reader, in a new READ COMMITTED
 *       transaction, sees the fresh value immediately.
 *    3. Write conflict — two SNAPSHOT transactions update the same row;
 *       the NO WAIT loser gets the classic update-conflict error chain
 *       (SQLSTATE 40001) instead of blocking.
 *
 *  Build & run (see ../README.md):
 *      ./transactions_demo [database]
 *  Default database: inet://localhost//tmp/fbhandson/tx.fdb (created on
 *  first run; /tmp/fbhandson must exist and be writable by the server).
 */

#include "fb_sample.h"

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/tx.fdb");

	try
	{
		// Two independent attachments: two "users".
		Db a, b;
		a.attachOrCreate(database);
		b.attach(database);

		// Seed one row, committed and visible to everyone.
		{
			ITransaction* t = a.start();
			a.exec(t, "recreate table balance (id integer primary key, amount integer)");
			t->commit(&a.status);
			t = a.start();
			a.exec(t, "insert into balance values (1, 100)");
			t->commit(&a.status);
		}

		// --- 1. SNAPSHOT stability -------------------------------------------
		ITransaction* snapA = a.start(tpb({isc_tpb_concurrency, isc_tpb_wait}));
		printf("A (SNAPSHOT)       sees amount = %s\n",
			a.queryValue(snapA, "select amount from balance where id = 1").c_str());

		ITransaction* txB = b.start();
		b.exec(txB, "update balance set amount = 999 where id = 1");
		txB->commit(&b.status);
		printf("B                  committed amount = 999\n");

		printf("A (same SNAPSHOT)  sees amount = %s   <- still the start-of-tx version\n",
			a.queryValue(snapA, "select amount from balance where id = 1").c_str());

		// --- 2. READ COMMITTED sees the new version --------------------------
		ITransaction* rcA = a.start(tpb({isc_tpb_read_committed,
			isc_tpb_rec_version, isc_tpb_wait}));
		printf("A (READ COMMITTED) sees amount = %s   <- the committed version\n",
			a.queryValue(rcA, "select amount from balance where id = 1").c_str());
		rcA->commit(&a.status);

		// --- 3. Write conflict under NO WAIT ---------------------------------
		// B updates the row and stays open; A (still in its old SNAPSHOT,
		// NO WAIT via a fresh conflicting transaction) tries the same row.
		ITransaction* holdB = b.start(tpb({isc_tpb_concurrency, isc_tpb_wait}));
		b.exec(holdB, "update balance set amount = amount + 1 where id = 1");

		ITransaction* loserA = a.start(tpb({isc_tpb_concurrency, isc_tpb_nowait}));
		try
		{
			a.exec(loserA, "update balance set amount = amount + 10 where id = 1");
			printf("unexpected: conflicting update succeeded\n");
		}
		catch (const FbException& e)
		{
			char buf[512];
			a.util->formatStatus(buf, sizeof(buf), e.getStatus());
			printf("A conflicting update failed as designed:\n    %s\n", buf);
		}
		loserA->rollback(&a.status);
		holdB->commit(&b.status);
		snapA->commit(&a.status);

		printf("done.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
