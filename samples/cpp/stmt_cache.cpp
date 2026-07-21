/*
 *  stmt_cache.cpp — companion to ../../statement-cache.md
 *
 *  There is no monitoring view of the DSQL statement cache, so — like the
 *  document's own demonstrations — this sample infers its behaviour from
 *  prepare timings on a statement that is heavy to *compile* (a six-way
 *  self-join: large join-order search) and trivial to execute.  Nothing is
 *  ever executed; every loop is prepare + free only.
 *
 *    run 1  identical text              -> all but the first prepare hit
 *    run 2  same SQL + i trailing spaces-> all miss: the key is the text
 *           verbatim, whitespace included (buildStatementKey)
 *    run 3  distinct literal each time  -> all miss, for comparison
 *    run 4  identical text, but each prepare preceded by an unrelated
 *           RECREATE TABLE + commit     -> all miss: any DDL commit purges
 *           the whole cache (dfw.epp -> purgeAllAttachments)
 *
 *  Uses its own scratch database — safe to re-run.
 */

#include "fb_sample.h"
#include <chrono>

using namespace Firebird;

static const char* HEAVY =
	"SELECT COUNT(*) FROM t a"
	" JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id"
	" JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id"
	" JOIN t f ON e.id = f.id WHERE a.id > 0";

static double ms(std::function<void()> f)
{
	const auto t0 = std::chrono::steady_clock::now();
	f();
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
}

static void prepareOnce(fbsample::Db& db, ITransaction* tra, const std::string& sql)
{
	IStatement* stmt = db.att->prepare(&db.status, tra, 0, sql.c_str(),
		SQL_DIALECT_V6, 0);
	stmt->free(&db.status);
}

int main(int argc, char** argv)
{
	const char* database = fbsample::argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/stmt_cache.fdb");
	const int N = 100;

	try
	{
		fbsample::Db db;
		db.attachOrCreate(database);

		ITransaction* setup = db.start();
		db.exec(setup, "RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)");
		setup->commitRetaining(&db.status);
		db.exec(setup,
			"EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO"
			" BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END");
		setup->commit(&db.status);

		ITransaction* tra = db.start();
		prepareOnce(db, tra, HEAVY);   // warm the cache with the exact text

		double t = ms([&] {
			for (int i = 0; i < N; ++i)
				prepareOnce(db, tra, HEAVY);
		});
		printf("1. identical text            %3d prepares: %6.1f ms  (%.2f ms/prepare) - hits\n",
			N, t, t / N);

		t = ms([&] {
			for (int i = 0; i < N; ++i)
				prepareOnce(db, tra, HEAVY + std::string(i + 1, ' '));
		});
		printf("2. + i trailing spaces       %3d prepares: %6.1f ms  (%.2f ms/prepare) - misses\n",
			N, t, t / N);

		t = ms([&] {
			for (int i = 0; i < N; ++i)
			{
				std::string sql(HEAVY);
				sql.replace(sql.find("> 0"), 3, "> " + std::to_string(i));
				prepareOnce(db, tra, sql);
			}
		});
		printf("3. distinct literal          %3d prepares: %6.1f ms  (%.2f ms/prepare) - misses\n",
			N, t, t / N);

		t = 0;   // time only the prepares, not the DDL itself
		for (int i = 0; i < N; ++i)
		{
			ITransaction* ddl = db.start();
			db.exec(ddl, "RECREATE TABLE unrelated (x INT)");
			ddl->commit(&db.status);   // purges every cache in the database
			t += ms([&] { prepareOnce(db, tra, HEAVY); });
		}
		printf("4. identical text after DDL  %3d prepares: %6.1f ms  (%.2f ms/prepare) - misses\n",
			N, t, t / N);

		tra->commit(&db.status);
	}
	catch (const FbException& error)
	{
		return fbsample::report(error);
	}
	return 0;
}
