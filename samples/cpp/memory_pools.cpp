/*
 *  memory_pools.cpp — companion sample for memory-management.md.
 *
 *  Makes the pool hierarchy visible from SQL via MON$MEMORY_USAGE:
 *
 *    - the per-level summary (six stat groups) with the parent-redirection
 *      signature: child pools showing real MON$MEMORY_USED and *zero*
 *      MON$MEMORY_ALLOCATED — every block borrowed from the database pool;
 *    - this connection's own database -> attachment -> transaction chain;
 *    - a transaction pool growing live while its transaction accumulates
 *      an undo log (an uncommitted 3000-row UPDATE), observed from a second
 *      attachment because MON$ snapshots are per-transaction.
 */

#include "fb_sample.h"

using namespace fbsample;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/memory_pools.fdb";

static void levelSummary(Db& db)
{
	ITransaction* t = db.start();
	Db::Table s = db.query(t,
		"select MON$STAT_GROUP, count(*), sum(MON$MEMORY_USED), "
		"       sum(MON$MEMORY_ALLOCATED), "
		"       count(nullif(MON$MEMORY_ALLOCATED, 0)) "
		"from MON$MEMORY_USAGE group by 1 order by 1");
	printf("stat_group (0=db 1=att 2=tra 3=stmt 5=cmp)  pools  used  allocated  with_own_extents\n");
	Db::print(s);
	t->commit(&db.status);
}

// One row of the caller's own pool chain, freshly snapshotted.
static void poolRow(Db& db, const char* label, const std::string& join)
{
	ITransaction* t = db.start();
	Db::Table r = db.query(t,
		"select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE " + join);
	if (!r.rows.empty())
		printf("  %-24s used=%-10s allocated=%s\n", label,
			r.rows[0][0].c_str(), r.rows[0][1].c_str());
	t->commit(&db.status);
}

int main(int argc, char** argv)
{
	const char* dbName = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Db worker, monitor;
		worker.attachOrCreate(dbName);
		monitor.attach(dbName);

		ITransaction* t = worker.start();
		try { worker.exec(t, "recreate table t (id int, pad varchar(200))"); }
		catch (const FbException&) {}
		t->commit(&worker.status);
		t = worker.start();
		worker.exec(t,
			"execute block as declare i int = 0; begin"
			"  while (i < 3000) do begin"
			"    insert into t values (:i, rpad('x', 200, 'x')); i = i + 1;"
			"  end "
			"end");
		t->commit(&worker.status);

		printf("-- per-level summary (note used > 0 with allocated = 0: parent redirection)\n");
		levelSummary(monitor);

		// The worker's own chain: database -> attachment -> transaction.
		t = worker.start();
		std::string att = worker.queryValue(t, "select current_connection from rdb$database");
		std::string tra = worker.queryValue(t, "select current_transaction from rdb$database");

		printf("\n-- worker's pool chain (before the update)\n");
		poolRow(monitor, "database pool:", "join MON$DATABASE using (MON$STAT_ID)");
		poolRow(monitor, "worker attachment pool:",
			"join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = " + att);
		poolRow(monitor, "worker transaction pool:",
			"join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = " + tra);

		// Grow the transaction pool: an uncommitted UPDATE of 3000 rows must
		// keep every old version in this transaction's undo log, and the
		// undo log lives in the transaction's pool.
		worker.exec(t, "update t set pad = rpad('y', 200, 'y')");

		printf("\n-- after an uncommitted 3000-row UPDATE in that transaction\n");
		poolRow(monitor, "worker attachment pool:",
			"join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = " + att);
		poolRow(monitor, "worker transaction pool:",
			"join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = " + tra);

		t->rollback(&worker.status);    // bulk-free: the whole pool goes at once
		printf("\n-- after rollback (transaction pool destroyed with its undo log)\n");
		poolRow(monitor, "worker attachment pool:",
			"join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = " + att);
		return 0;
	}
	catch (const FbException& e) { return report(e); }
}
