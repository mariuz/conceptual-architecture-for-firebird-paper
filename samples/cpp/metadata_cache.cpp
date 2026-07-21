/*
 *  metadata_cache.cpp — companion to ../../metadata-cache.md
 *
 *  The visibility rule of the metadata cache, exercised from two
 *  attachments (A and B) on one scratch database:
 *
 *    1. an uncommitted ALTER is visible to its own transaction
 *       (traNumber == currentTrans) and to nobody else — B gets
 *       "Column unknown" while A already selects the new column;
 *    2. once A commits, a committed version is visible to everyone
 *       IMMEDIATELY — even to a statement prepared inside B's older,
 *       still-open SNAPSHOT transaction: metadata is read-committed,
 *       not snapshot-isolated;
 *    3. two concurrent uncommitted DDLs on the same object collide in
 *       CacheElement::newVersion — the engine's "object in use" error,
 *       naming the MetaId and the blocking transaction;
 *    4. every ALTER appended a row to RDB$FORMATS: old records decode
 *       lazily (NULL in columns that postdate them).
 */

#include "fb_sample.h"

using namespace fbsample;

// Run a query, printing either the first-row/first-column value or the error.
static void tryQuery(const char* who, Db& db, ITransaction* tra, const char* sql)
{
	try
	{
		printf("%s: %s -> %s\n", who, sql, db.queryValue(tra, sql).c_str());
	}
	catch (const FbException& e)
	{
		char buf[512];
		master->getUtilInterface()->formatStatus(buf, sizeof buf, e.getStatus());
		for (char* c = buf; *c; ++c)
			if (*c == '\n') *c = ' ';
		printf("%s: %s -> ERROR: %s\n", who, sql, buf);
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost//tmp/fbhandson/mdc.fdb");

	try
	{
		Db A, B;
		A.attachOrCreate(database);
		B.attach(database);

		ITransaction* t = A.start();
		A.exec(t, "recreate table t (a integer)");
		t->commit(&A.status);
		t = A.start();
		A.exec(t, "insert into t values (1)");
		t->commit(&A.status);

		// -- 1. uncommitted DDL: mine, and mine alone -----------------------
		printf("== 1. uncommitted ALTER: visible to creator only ==\n");
		ITransaction* aDdl = A.start();				// A's DDL stays uncommitted
		A.exec(aDdl, "alter table t add e integer");
		tryQuery("A (same tx)  ", A, aDdl, "select e from t");
		ITransaction* bTra = B.start();
		tryQuery("B            ", B, bTra, "select e from t");
		bTra->commit(&B.status);

		// -- 2. committed DDL ignores open snapshots ------------------------
		printf("\n== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==\n");
		bTra = B.start(tpb({isc_tpb_concurrency}));	// explicit SNAPSHOT
		tryQuery("B (snapshot) ", B, bTra, "select count(*) from t");
		aDdl->commit(&A.status);					// E becomes committed
		t = A.start();
		A.exec(t, "alter table t add d integer");
		t->commit(&A.status);						// D committed after B's snapshot
		tryQuery("B (same  tx) ", B, bTra, "select d from t");
		printf("   (records are snapshot-isolated; metadata is read-committed —\n"
			"    the new statement was prepared against the chain's current head)\n");
		bTra->commit(&B.status);

		// -- 3. concurrent DDL: the newVersion collision --------------------
		printf("\n== 3. two uncommitted DDLs on one object ==\n");
		aDdl = A.start();
		A.exec(aDdl, "alter table t add f integer");
		bTra = B.start();
		try
		{
			B.exec(bTra, "alter table t add g integer");
			printf("B: ALTER unexpectedly succeeded\n");
		}
		catch (const FbException& e)
		{
			char buf[512];
			master->getUtilInterface()->formatStatus(buf, sizeof buf, e.getStatus());
			printf("B: ALTER failed:\n%s\n", buf);
		}
		bTra->rollback(&B.status);
		aDdl->rollback(&A.status);					// F vanishes with the rollback

		// -- 4. the on-disk half: one format per committed shape ------------
		printf("\n== 4. RDB$FORMATS after the committed DDL ==\n");
		t = A.start();
		printf("formats stored for T: %s (T has lived through that many shapes)\n",
			A.queryValue(t, "select count(*) from rdb$formats f "
				"join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id "
				"where r.rdb$relation_name = 'T'").c_str());
		Db::print(A.query(t, "select a, e, d from t"));
		t->commit(&A.status);

		A.att->detach(&A.status); A.att = nullptr;
		B.att->detach(&B.status); B.att = nullptr;
		printf("done.\n");
		return 0;
	}
	catch (const FbException& error)
	{
		return report(error);
	}
}
