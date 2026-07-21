/*
 *  memory_pools.cpp (fb-cpp) — the pool hierarchy via MON$MEMORY_USAGE.
 *
 *  The fb-cpp twin of ../cpp/memory_pools.cpp: the six-group summary with
 *  the parent-redirection signature (used > 0, allocated = 0), this
 *  connection's database -> attachment -> transaction pool chain, and a
 *  transaction pool growing under an uncommitted 3000-row UPDATE — watched
 *  from a second attachment, because MON$ snapshots are per-transaction.
 *  Where the OO-API version splices attachment/transaction ids into the SQL
 *  text, fb-cpp passes them as typed parameters (a std::tuple bound to the
 *  `?` placeholder), and each usage row lands in a struct via
 *  queryFirstRowAs<PoolUsage>().
 *  See ../../memory-management.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_memory_pools [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/memory_pools_fbcpp.fdb";

struct PoolUsage
{
	std::int64_t used, allocated;
};

static void levelSummary(Attachment& att)
{
	Transaction t{att};
	Statement stmt{att, t,
		"select MON$STAT_GROUP, count(*), sum(MON$MEMORY_USED), "
		"       sum(MON$MEMORY_ALLOCATED), "
		"       count(nullif(MON$MEMORY_ALLOCATED, 0)) "
		"from MON$MEMORY_USAGE group by 1 order by 1"};
	printf("stat_group (0=db 1=att 2=tra 3=stmt 5=cmp)  pools  used  allocated  with_own_extents\n");

	const auto& cols = stmt.getOutputDescriptors();
	std::vector<std::string> names;
	for (const auto& d : cols)
		names.push_back(d.alias.empty() ? d.name : d.alias);
	std::vector<std::vector<std::string>> rows;
	// execute() already fetches the first row of a SELECT cursor
	for (bool more = stmt.execute(t); more; more = stmt.fetchNext())
	{
		std::vector<std::string> row;
		for (unsigned i = 0; i < cols.size(); ++i)
			row.push_back(stmt.getString(i).value_or("<null>"));
		rows.push_back(std::move(row));
	}
	std::vector<size_t> w;
	for (size_t c = 0; c < names.size(); ++c)
	{
		size_t m = names[c].size();
		for (auto& r : rows)
			m = std::max(m, r[c].size());
		w.push_back(m);
	}
	for (size_t c = 0; c < names.size(); ++c)
		printf("%-*s ", (int) w[c], names[c].c_str());
	printf("\n");
	for (size_t c = 0; c < names.size(); ++c)
		printf("%s ", std::string(w[c], '-').c_str());
	printf("\n");
	for (auto& r : rows)
	{
		for (size_t c = 0; c < r.size(); ++c)
			printf("%-*s ", (int) w[c], r[c].c_str());
		printf("\n");
	}
	t.commit();
}

// One row of the caller's own pool chain, freshly snapshotted; the id (when
// present) travels as a typed parameter, not as spliced SQL text.
static void poolRow(Attachment& att, const char* label, const std::string& sql,
	std::optional<std::int64_t> id = {})
{
	Transaction t{att};
	const auto r = id
		? att.queryFirstRowAs<PoolUsage>(t, sql, std::make_tuple(id.value()))
		: att.queryFirstRowAs<PoolUsage>(t, sql);
	if (r)
		printf("  %-24s used=%-10lld allocated=%lld\n", label,
			(long long) r->used, (long long) r->allocated);
	t.commit();
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Client client{"fbclient"};
		Attachment worker = attachOrCreate(client, database);
		Attachment monitor{client, database, defaultOptions()};

		{
			Transaction t{worker};
			worker.execute(t, "recreate table t (id int, pad varchar(200))");
			t.commit();
			Transaction fill{worker};
			worker.execute(fill,
				"execute block as declare i int = 0; begin"
				"  while (i < 3000) do begin"
				"    insert into t values (:i, rpad('x', 200, 'x')); i = i + 1;"
				"  end "
				"end");
			fill.commit();
		}

		printf("-- per-level summary (note used > 0 with allocated = 0: parent redirection)\n");
		levelSummary(monitor);

		// The worker's own chain: database -> attachment -> transaction.
		Transaction t{worker};
		const auto att = worker.queryScalar<std::int64_t>(t,
			"select current_connection from rdb$database").value();
		const auto tra = worker.queryScalar<std::int64_t>(t,
			"select current_transaction from rdb$database").value();

		const std::string attSql =
			"select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE "
			"join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ?";
		const std::string traSql =
			"select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE "
			"join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = ?";

		printf("\n-- worker's pool chain (before the update)\n");
		poolRow(monitor, "database pool:",
			"select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE "
			"join MON$DATABASE using (MON$STAT_ID)");
		poolRow(monitor, "worker attachment pool:", attSql, att);
		poolRow(monitor, "worker transaction pool:", traSql, tra);

		// Grow the transaction pool: an uncommitted UPDATE of 3000 rows must
		// keep every old version in this transaction's undo log, and the
		// undo log lives in the transaction's pool.
		worker.execute(t, "update t set pad = rpad('y', 200, 'y')");

		printf("\n-- after an uncommitted 3000-row UPDATE in that transaction\n");
		poolRow(monitor, "worker attachment pool:", attSql, att);
		poolRow(monitor, "worker transaction pool:", traSql, tra);

		t.rollback();       // bulk-free: the whole pool goes at once
		printf("\n-- after rollback (transaction pool destroyed with its undo log)\n");
		poolRow(monitor, "worker attachment pool:", attSql, att);
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
