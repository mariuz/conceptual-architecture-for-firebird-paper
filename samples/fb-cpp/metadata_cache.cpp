/*
 *  metadata_cache.cpp (fb-cpp) — companion to ../../metadata-cache.md
 *
 *  The fb-cpp twin of ../cpp/metadata_cache.cpp: the same four
 *  demonstrations of the visibility rule from two attachments A and B —
 *  uncommitted DDL visible to its creator only, committed DDL ignoring
 *  B's open SNAPSHOT (declared as a typed option instead of a raw
 *  isc_tpb_concurrency byte), the newVersion collision between two
 *  uncommitted DDLs, and RDB$FORMATS counting the table's shapes.  Errors
 *  arrive as typed DatabaseException objects whose what() already carries
 *  the formatted status chain — no IUtil::formatStatus needed.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_metadata_cache [database]
 */

#include "fbcpp_sample.h"
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

// Run a query, printing either the first-row/first-column value or the error.
static void tryQuery(const char* who, Attachment& att, Transaction& tra, const char* sql)
{
	try
	{
		Statement stmt{att, tra, sql};
		stmt.execute(tra);
		printf("%s: %s -> %s\n", who, sql, stmt.getString(0).value_or("<null>").c_str());
	}
	catch (const DatabaseException& e)
	{
		std::string msg = e.what();
		for (char& c : msg)
			if (c == '\n') c = ' ';
		printf("%s: %s -> ERROR: %s\n", who, sql, msg.c_str());
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/mdc_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment A = attachOrCreate(client, database);
		Attachment B{client, database, defaultOptions()};

		{
			Transaction t{A};
			A.execute(t, "recreate table t (a integer)");
			t.commit();
		}
		{
			Transaction t{A};
			A.execute(t, "insert into t values (1)");
			t.commit();
		}

		// -- 1. uncommitted DDL: mine, and mine alone -----------------------
		printf("== 1. uncommitted ALTER: visible to creator only ==\n");
		Transaction aDdl{A};						// A's DDL stays uncommitted
		A.execute(aDdl, "alter table t add e integer");
		tryQuery("A (same tx)  ", A, aDdl, "select e from t");
		{
			Transaction bTra{B};
			tryQuery("B            ", B, bTra, "select e from t");
			bTra.commit();
		}

		// -- 2. committed DDL ignores open snapshots ------------------------
		printf("\n== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==\n");
		Transaction bSnap{B, TransactionOptions()
			.setIsolationLevel(TransactionIsolationLevel::SNAPSHOT)};
		tryQuery("B (snapshot) ", B, bSnap, "select count(*) from t");
		aDdl.commit();								// E becomes committed
		{
			Transaction t{A};
			A.execute(t, "alter table t add d integer");
			t.commit();								// D committed after B's snapshot
		}
		tryQuery("B (same  tx) ", B, bSnap, "select d from t");
		printf("   (records are snapshot-isolated; metadata is read-committed —\n"
			"    the new statement was prepared against the chain's current head)\n");
		bSnap.commit();

		// -- 3. concurrent DDL: the newVersion collision --------------------
		printf("\n== 3. two uncommitted DDLs on one object ==\n");
		Transaction aDdl2{A};
		A.execute(aDdl2, "alter table t add f integer");
		Transaction bTra{B};
		try
		{
			B.execute(bTra, "alter table t add g integer");
			printf("B: ALTER unexpectedly succeeded\n");
		}
		catch (const DatabaseException& e)
		{
			printf("B: ALTER failed (gds %ld):\n%s\n",
				(long) e.getErrorCode(), e.what());
		}
		bTra.rollback();
		aDdl2.rollback();							// F vanishes with the rollback

		// -- 4. the on-disk half: one format per committed shape ------------
		printf("\n== 4. RDB$FORMATS after the committed DDL ==\n");
		Transaction t{A};
		Statement cnt{A, t, "select count(*) from rdb$formats f "
			"join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id "
			"where r.rdb$relation_name = 'T'"};
		cnt.execute(t);
		printf("formats stored for T: %lld (T has lived through that many shapes)\n",
			(long long) cnt.getInt64(0).value_or(0));
		Statement row{A, t, "select a, e, d from t"};
		printf("A E      D\n- ------ ------\n");
		for (bool more = row.execute(t); more; more = row.fetchNext())
			printf("%-1s %-6s %-6s\n",
				row.getString(0).value_or("<null>").c_str(),
				row.getString(1).value_or("<null>").c_str(),
				row.getString(2).value_or("<null>").c_str());
		t.commit();

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
