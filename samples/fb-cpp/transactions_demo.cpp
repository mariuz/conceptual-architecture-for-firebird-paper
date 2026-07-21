/*
 *  transactions_demo.cpp (fb-cpp) — MVCC isolation seen from the client.
 *
 *  The fb-cpp twin of ../cpp/transactions_demo.cpp: the same scenario —
 *  SNAPSHOT stability, READ COMMITTED freshness, NO WAIT write conflict —
 *  with fb-cpp's typed option objects replacing hand-built TPBs and
 *  std::optional replacing null indicators.  Compare the TPB constants in
 *  the OO-API version with TransactionOptions here: setIsolationLevel /
 *  setReadCommittedMode / setWaitMode are the same three TPB decisions
 *  with names.  See ../../transactions-and-concurrency.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_transactions_demo [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

static int amountSeenBy(Attachment& att, Transaction& tra)
{
	Statement stmt{att, tra, "select amount from balance where id = 1"};
	stmt.execute(tra);
	return stmt.getInt32(0).value_or(-1);
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/tx_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};

		// Two independent attachments: two "users".
		Attachment a = attachOrCreate(client, database);
		Attachment b{client, database, defaultOptions()};

		// Seed one row, committed and visible to everyone.
		{
			Transaction t{a};
			Statement{a, t, "recreate table balance (id integer primary key, amount integer)"}
				.execute(t);
			t.commit();
			Transaction t2{a};
			Statement{a, t2, "insert into balance values (1, 100)"}.execute(t2);
			t2.commit();
		}

		// --- 1. SNAPSHOT stability -------------------------------------------
		Transaction snapA{a, TransactionOptions()
			.setIsolationLevel(TransactionIsolationLevel::SNAPSHOT)};
		printf("A (SNAPSHOT)       sees amount = %d\n", amountSeenBy(a, snapA));

		{
			Transaction txB{b};
			Statement{b, txB, "update balance set amount = 999 where id = 1"}.execute(txB);
			txB.commit();
		}
		printf("B                  committed amount = 999\n");
		printf("A (same SNAPSHOT)  sees amount = %d   <- still the start-of-tx version\n",
			amountSeenBy(a, snapA));

		// --- 2. READ COMMITTED sees the new version --------------------------
		Transaction rcA{a, TransactionOptions()
			.setIsolationLevel(TransactionIsolationLevel::READ_COMMITTED)
			.setReadCommittedMode(TransactionReadCommittedMode::RECORD_VERSION)};
		printf("A (READ COMMITTED) sees amount = %d   <- the committed version\n",
			amountSeenBy(a, rcA));
		rcA.commit();

		// --- 3. Write conflict under NO WAIT ---------------------------------
		Transaction holdB{b, TransactionOptions()
			.setIsolationLevel(TransactionIsolationLevel::SNAPSHOT)};
		Statement{b, holdB, "update balance set amount = amount + 1 where id = 1"}
			.execute(holdB);

		Transaction loserA{a, TransactionOptions()
			.setIsolationLevel(TransactionIsolationLevel::SNAPSHOT)
			.setWaitMode(TransactionWaitMode::NO_WAIT)};
		try
		{
			Statement{a, loserA, "update balance set amount = amount + 10 where id = 1"}
				.execute(loserA);
			printf("unexpected: conflicting update succeeded\n");
		}
		catch (const DatabaseException& e)
		{
			printf("A conflicting update failed as designed (gds %ld = isc_deadlock):\n    %s\n",
				static_cast<long>(e.getErrorCode()), e.what());
		}
		loserA.rollback();
		holdB.commit();
		snapA.commit();

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
