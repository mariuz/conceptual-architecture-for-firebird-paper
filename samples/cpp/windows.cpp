/*
 *  windows.cpp — companion sample for aggregate-and-window-functions.md.
 *
 *  Recreates the document's six-row sales table and runs its flagship
 *  analytics live: the ranking/frame/LAG window query, FILTER + LISTAGG +
 *  STDDEV_POP aggregates, PERCENTILE_CONT and a hypothetical-set
 *  RANK ... WITHIN GROUP, plus a Firebird 6 frame with EXCLUDE CURRENT ROW
 *  (average of the two neighbours, current row left out).  For the window
 *  query it also prints the legacy plan, showing the SORT the document's
 *  execution section attributes to SortedStream -> WindowedStream.
 *
 *  Build/run: see ../../aggregate-and-window-functions.md (Hands-on).
 */

#include "fb_sample.h"

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/windows.fdb");
	try
	{
		Db db;
		db.attachOrCreate(database);

		ITransaction* ddl = db.start();
		try { db.exec(ddl, "DROP TABLE sales"); } catch (const FbException&) {}
		db.exec(ddl,
			"CREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(10),"
			" amount NUMERIC(10,2))");
		ddl->commit(&db.status);

		ITransaction* tra = db.start();
		const char* rows[] = {
			"(1,'East',100)", "(2,'East',200)", "(3,'East',150)",
			"(4,'West',300)", "(5,'West',250)", "(6,'West',400)",
		};
		for (const char* r : rows)
			db.exec(tra, std::string("INSERT INTO sales VALUES ") + r);

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
		Db::print(db.query(tra, winSql));

		IStatement* stmt = db.att->prepare(&db.status, tra, 0, winSql,
			SQL_DIALECT_V6, 0);
		printf("\nplan:%s\n", stmt->getPlan(&db.status, false));
		stmt->free(&db.status);

		// -- 2. Aggregates: FILTER (FB5), ordered LISTAGG, statistical.
		printf("\n== aggregates: FILTER / LISTAGG / STDDEV_POP ==\n");
		Db::print(db.query(tra,
			"SELECT region, COUNT(*) AS n,"
			" COUNT(*) FILTER (WHERE amount > 150) AS big_sales,"
			" CAST(LISTAGG(amount, ',') WITHIN GROUP (ORDER BY amount)"
			"   AS VARCHAR(60)) AS amounts,"
			" CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev"
			" FROM sales GROUP BY region"));

		// -- 3. Ordered-set and hypothetical-set aggregates: the median,
		//       and "what rank would a 175 sale have in each region?"
		printf("\n== PERCENTILE_CONT median / hypothetical RANK(175) ==\n");
		Db::print(db.query(tra,
			"SELECT region,"
			" PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median,"
			" RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175"
			" FROM sales GROUP BY region"));

		// -- 4. FB6 frame exclusion: each row's neighbours' average,
		//       the row itself EXCLUDEd from its own frame.
		printf("\n== FB6 frame EXCLUDE CURRENT ROW (neighbours' average) ==\n");
		Db::print(db.query(tra,
			"SELECT id, amount,"
			" CAST(AVG(amount) OVER (ORDER BY id"
			"   ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING"
			"   EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg"
			" FROM sales"));

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
