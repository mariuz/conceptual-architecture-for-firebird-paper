/*
 *  plans.cpp (fb-cpp) — watching the optimizer decide, one abstraction up.
 *
 *  The fb-cpp twin of ../cpp/plans.cpp: the same statements are prepared
 *  (never executed) and both plan forms printed.  The OO API's
 *  IStatement::getPlan(detailed) pair becomes two typed methods —
 *  Statement::getLegacyPlan() / Statement::getPlan() — and the
 *  StatementOptions builder asks the server to send both plans along with
 *  the prepare round trip (setPrefetchLegacyPlan / setPrefetchPlan map to
 *  PREPARE_PREFETCH_LEGACY_PLAN / _DETAILED_PLAN).
 *  See ../../query-optimizer-and-execution.md.
 *
 *  Uses its own scratch database — safe to re-run.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_plans [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static void exec(Attachment& att, Transaction& tra, const char* sql)
{
	Statement{att, tra, sql}.execute(tra);
}

static void showPlan(Attachment& att, Transaction& tra, const char* sql)
{
	printf("== %s\n", sql);
	Statement stmt{att, tra, sql, StatementOptions()
		.setPrefetchLegacyPlan(true)
		.setPrefetchPlan(true)};
	std::string legacy = stmt.getLegacyPlan();
	const std::string detailed = stmt.getPlan();
	legacy.erase(0, legacy.find_first_not_of('\n'));   // both open with a newline
	printf("legacy:  %s\ndetailed:%s\n\n", legacy.c_str(), detailed.c_str());
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/plans_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		// -- Build the schema: 20 departments, 2000 employees. ------------
		{
			Transaction tra{att};
			exec(att, tra, "RECREATE TABLE dept (id INT NOT NULL PRIMARY KEY,"
				" name VARCHAR(20))");
			exec(att, tra, "RECREATE TABLE emp (id INT NOT NULL PRIMARY KEY,"
				" dept_id INT, salary INT, name VARCHAR(20))");
			tra.commitRetaining();
			exec(att, tra,
				"EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN\n"
				"  WHILE (i <= 20) DO BEGIN\n"
				"    INSERT INTO dept VALUES (:i, 'dept ' || :i); i = i + 1;\n"
				"  END\n"
				"  i = 1;\n"
				"  WHILE (i <= 2000) DO BEGIN\n"
				"    INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,\n"
				"        1000 + MOD(:i * 37, 500), 'emp ' || :i); i = i + 1;\n"
				"  END\n"
				"END");
			tra.commit();
		}

		// -- 1. No index on dept_id yet: the full scan is the only path. --
		{
			Transaction tra{att};
			showPlan(att, tra, "SELECT name FROM emp WHERE dept_id = 5");

			// -- 2. Create the index; the same text compiles differently. -
			exec(att, tra, "CREATE INDEX emp_dept ON emp (dept_id)");
			tra.commit();
		}

		Transaction tra{att};
		printf("-- CREATE INDEX emp_dept ON emp (dept_id) --\n\n");
		showPlan(att, tra, "SELECT name FROM emp WHERE dept_id = 5");

		// -- 3. PK equality: unique index, nothing cheaper than one row. --
		showPlan(att, tra, "SELECT name FROM emp WHERE id = 42");

		// -- 4. Join + ORDER BY: SORT over a nested loop with the index. --
		showPlan(att, tra,
			"SELECT e.name, d.name FROM emp e"
			" JOIN dept d ON e.dept_id = d.id ORDER BY e.salary");

		// -- 5. Equi-join with no usable index on either side: hash join. -
		showPlan(att, tra,
			"SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary");

		tra.commit();
		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
