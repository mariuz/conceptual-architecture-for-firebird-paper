/*
 *  windows.cpp (fb-cpp) — the document's analytics, fetched typed.
 *
 *  The fb-cpp twin of ../cpp/windows.cpp: the same six-row sales table and
 *  the same flagship queries — the ranking/frame/LAG window query (with its
 *  legacy plan, showing the SORT behind SortedStream -> WindowedStream),
 *  FILTER + LISTAGG + STDDEV_POP aggregates, PERCENTILE_CONT and the
 *  hypothetical-set RANK ... WITHIN GROUP, and the Firebird 6 frame with
 *  EXCLUDE CURRENT ROW.  Where the OO-API sample coerced whole result sets
 *  to VARCHAR for its generic printer, this one's printer walks fb-cpp's
 *  cached Descriptors for the column names and lets getString() render each
 *  value client-side — and LAG's NULL in the first row of every partition
 *  arrives as an empty std::optional, not a sentinel string.
 *  See ../../aggregate-and-window-functions.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_windows [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace fbcpp;
using namespace fbcpp_sample;

static void exec(Attachment& att, Transaction& tra, const std::string& sql)
{
	Statement{att, tra, sql}.execute(tra);
}

// Generic printer: SELECT-list aliases from the Descriptors, values via
// getString(), NULLs (empty optionals) shown as "<null>".
static void print(Attachment& att, Transaction& tra, const char* sql)
{
	Statement stmt{att, tra, sql};
	const auto& descs = stmt.getOutputDescriptors();
	for (const auto& d : descs)
		printf("%-14s ", d.alias.c_str());
	printf("\n");
	for (const auto& d : descs)
		printf("%-14s ", std::string(d.alias.size(), '-').c_str());
	printf("\n");
	for (bool ok = stmt.execute(tra); ok; ok = stmt.fetchNext())
	{
		for (unsigned i = 0; i < descs.size(); ++i)
			printf("%-14s ", stmt.getString(i).value_or("<null>").c_str());
		printf("\n");
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/windows_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		{
			Transaction ddl{att};
			try { exec(att, ddl, "DROP TABLE sales"); } catch (const DatabaseException&) {}
			exec(att, ddl,
				"CREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(10),"
				" amount NUMERIC(10,2))");
			ddl.commit();
		}

		Transaction tra{att};
		for (const char* r : {
				"(1,'East',100)", "(2,'East',200)", "(3,'East',150)",
				"(4,'West',300)", "(5,'West',250)", "(6,'West',400)" })
			exec(att, tra, std::string("INSERT INTO sales VALUES ") + r);

		// -- 1. The flagship window query: partitioned ranking, a framed
		//       running total, and LAG navigation — every row kept.
		const char* winSql =
			"SELECT region, amount,"
			" ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount) AS rn,"
			" RANK() OVER (ORDER BY amount DESC) AS overall_rank,"
			" SUM(amount) OVER (PARTITION BY region ORDER BY id"
			"   ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_total,"
			" LAG(amount) OVER (PARTITION BY region ORDER BY id) AS prev_amount"
			" FROM sales";
		printf("== window functions ==\n");
		print(att, tra, winSql);

		{
			Statement stmt{att, tra, winSql,
				StatementOptions().setPrefetchLegacyPlan(true)};
			printf("\nplan:%s\n", stmt.getLegacyPlan().c_str());
		}

		// -- 2. Aggregates: FILTER (FB5), ordered LISTAGG, statistical.
		printf("\n== aggregates: FILTER / LISTAGG / STDDEV_POP ==\n");
		print(att, tra,
			"SELECT region, COUNT(*) AS n,"
			" COUNT(*) FILTER (WHERE amount > 150) AS big_sales,"
			" CAST(LISTAGG(amount, ',') WITHIN GROUP (ORDER BY amount)"
			"   AS VARCHAR(60)) AS amounts,"
			" CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev"
			" FROM sales GROUP BY region");

		// -- 3. Ordered-set and hypothetical-set aggregates: the median,
		//       and "what rank would a 175 sale have in each region?"
		printf("\n== PERCENTILE_CONT median / hypothetical RANK(175) ==\n");
		print(att, tra,
			"SELECT region,"
			" PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median,"
			" RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175"
			" FROM sales GROUP BY region");

		// -- 4. FB6 frame exclusion: each row's neighbours' average,
		//       the row itself EXCLUDEd from its own frame.
		printf("\n== FB6 frame EXCLUDE CURRENT ROW (neighbours' average) ==\n");
		print(att, tra,
			"SELECT id, amount,"
			" CAST(AVG(amount) OVER (ORDER BY id"
			"   ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING"
			"   EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg"
			" FROM sales");

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
