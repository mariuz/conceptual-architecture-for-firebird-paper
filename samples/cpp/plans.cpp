/*
 *  plans.cpp — companion to ../../query-optimizer-and-execution.md
 *
 *  Watches the cost-based optimizer decide, from the client, through
 *  IStatement::getPlan(detailed): every statement is prepared (never
 *  executed!) and both plan forms are printed —
 *    - legacy:   PLAN (EMP INDEX (EMP_DEPT))          — the terse tree
 *    - detailed: -> Table ... Access By ID -> Bitmap  — the record-source
 *                tree of src/jrd/recsrc/, one line per operator.
 *  The same SELECT is prepared before and after CREATE INDEX, so the plan
 *  flip from a Full Scan to a Bitmap+Index Range Scan is visible; a join
 *  with ORDER BY shows SORT on top of a nested loop, and an indexless
 *  equi-join shows the Firebird 5 hash join.
 *
 *  Uses its own scratch database — safe to re-run.
 */

#include "fb_sample.h"

using namespace Firebird;

static void showPlan(fbsample::Db& db, ITransaction* tra, const char* sql)
{
	printf("== %s\n", sql);
	IStatement* stmt = db.att->prepare(&db.status, tra, 0, sql,
		SQL_DIALECT_V6, 0);   // no metadata prefetch needed for plans
	const char* legacy = stmt->getPlan(&db.status, false);
	const char* detailed = stmt->getPlan(&db.status, true);
	while (*legacy == '\n') ++legacy;       // both strings open with a newline
	printf("legacy:  %s\ndetailed:%s\n\n", legacy, detailed);
	stmt->free(&db.status);
}

int main(int argc, char** argv)
{
	const char* database = fbsample::argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/plans.fdb");

	try
	{
		fbsample::Db db;
		db.attachOrCreate(database);

		// -- Build the schema: 20 departments, 2000 employees. ------------
		ITransaction* tra = db.start();
		db.exec(tra, "RECREATE TABLE dept (id INT NOT NULL PRIMARY KEY,"
			" name VARCHAR(20))");
		db.exec(tra, "RECREATE TABLE emp (id INT NOT NULL PRIMARY KEY,"
			" dept_id INT, salary INT, name VARCHAR(20))");
		tra->commitRetaining(&db.status);
		db.exec(tra,
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
		tra->commit(&db.status);

		// -- 1. No index on dept_id yet: the full scan is the only path. --
		tra = db.start();
		showPlan(db, tra, "SELECT name FROM emp WHERE dept_id = 5");

		// -- 2. Create the index; the same text now compiles differently. -
		db.exec(tra, "CREATE INDEX emp_dept ON emp (dept_id)");
		tra->commit(&db.status);
		tra = db.start();
		printf("-- CREATE INDEX emp_dept ON emp (dept_id) --\n\n");
		showPlan(db, tra, "SELECT name FROM emp WHERE dept_id = 5");

		// -- 3. PK equality: unique index, nothing cheaper than one row. --
		showPlan(db, tra, "SELECT name FROM emp WHERE id = 42");

		// -- 4. Join + ORDER BY: SORT over a nested loop with the index. --
		showPlan(db, tra,
			"SELECT e.name, d.name FROM emp e"
			" JOIN dept d ON e.dept_id = d.id ORDER BY e.salary");

		// -- 5. Equi-join with no usable index on either side: hash join. -
		showPlan(db, tra,
			"SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary");

		tra->commit(&db.status);
		printf("done.\n");
	}
	catch (const FbException& error)
	{
		return fbsample::report(error);
	}
	return 0;
}
