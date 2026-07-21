/*
 *  stmt_cache.cpp (fb-cpp) — inferring the statement cache from timings.
 *
 *  The fb-cpp twin of ../cpp/stmt_cache.cpp: the same four timing runs on a
 *  compile-heavy six-way self-join, never executed.  In fb-cpp the whole
 *  prepare/free pair the OO-API sample spells out is one statement lifetime:
 *  constructing a Statement prepares (isc_dsql_sql_info metadata included),
 *  letting it go out of scope frees — so "prepareOnce" is just a braced
 *  temporary.  The server-side DSQL statement cache neither knows nor cares
 *  which client wrapper is calling: the key is still the text verbatim.
 *  See ../../statement-cache.md.
 *
 *  Uses its own scratch database — safe to re-run.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_stmt_cache [database]
 */

#include "fbcpp_sample.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* HEAVY =
	"SELECT COUNT(*) FROM t a"
	" JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id"
	" JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id"
	" JOIN t f ON e.id = f.id WHERE a.id > 0";

static double ms(const std::function<void()>& f)
{
	const auto t0 = std::chrono::steady_clock::now();
	f();
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
}

static void prepareOnce(Attachment& att, Transaction& tra, const std::string& sql)
{
	Statement{att, tra, sql};   // prepare in the ctor, free in the dtor
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/stmt_cache_fbcpp.fdb");
	const int N = 100;

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		{
			Transaction setup{att};
			Statement{att, setup, "RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)"}
				.execute(setup);
			setup.commitRetaining();
			Statement{att, setup,
				"EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO"
				" BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END"}
				.execute(setup);
			setup.commit();
		}

		Transaction tra{att};
		prepareOnce(att, tra, HEAVY);   // warm the cache with the exact text

		double t = ms([&] {
			for (int i = 0; i < N; ++i)
				prepareOnce(att, tra, HEAVY);
		});
		printf("1. identical text            %3d prepares: %6.1f ms  (%.2f ms/prepare) - hits\n",
			N, t, t / N);

		t = ms([&] {
			for (int i = 0; i < N; ++i)
				prepareOnce(att, tra, HEAVY + std::string(i + 1, ' '));
		});
		printf("2. + i trailing spaces       %3d prepares: %6.1f ms  (%.2f ms/prepare) - misses\n",
			N, t, t / N);

		t = ms([&] {
			for (int i = 0; i < N; ++i)
			{
				std::string sql(HEAVY);
				sql.replace(sql.find("> 0"), 3, "> " + std::to_string(i));
				prepareOnce(att, tra, sql);
			}
		});
		printf("3. distinct literal          %3d prepares: %6.1f ms  (%.2f ms/prepare) - misses\n",
			N, t, t / N);

		t = 0;   // time only the prepares, not the DDL itself
		for (int i = 0; i < N; ++i)
		{
			Transaction ddl{att};
			Statement{att, ddl, "RECREATE TABLE unrelated (x INT)"}.execute(ddl);
			ddl.commit();   // purges every cache in the database
			t += ms([&] { prepareOnce(att, tra, HEAVY); });
		}
		printf("4. identical text after DDL  %3d prepares: %6.1f ms  (%.2f ms/prepare) - misses\n",
			N, t, t / N);

		tra.commit();
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
