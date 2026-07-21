/*
 *  indexes.cpp — companion to ../../indexing-and-full-text-search.md
 *
 *  One B-tree, many variants: builds a 3,000-row table, creates a
 *  descending, an expression (COMPUTED BY), a partial (WHERE) and a plain
 *  index, then prepares five queries and prints the optimizer's plan for
 *  each (IStatement::getPlan).  The plans prove: expression predicate ->
 *  expression index, matching filter -> partial index, ORDER BY DESC ->
 *  descending index navigation, OR across two columns -> two indexes
 *  bitmap-combined (inversion), and CONTAINING -> NATURAL full scan —
 *  the no-native-full-text gap the document ends on.
 */

#include "fb_sample.h"

using namespace fbsample;

static void plan(Db& db, ITransaction* tra, const char* sql)
{
	IStatement* stmt = db.att->prepare(&db.status, tra, 0, sql, SQL_DIALECT_V6, 0);
	const char* p = stmt->getPlan(&db.status, false);	// false = legacy one-line plan
	printf("%s\n%s\n\n", sql, p ? p + (*p == '\n') : "(no plan)");
	stmt->free(&db.status);
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost//tmp/fbhandson/indexes.fdb");

	try
	{
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();
		db.exec(tra, "recreate table doc ("
			" id integer, title varchar(60), status varchar(10), num integer)");
		tra->commit(&db.status);

		tra = db.start();
		db.exec(tra,
			"execute block as declare i integer = 0; begin"
			"  while (i < 3000) do begin"
			"    insert into doc values (:i, 'Title ' || :i,"
			"      iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100));"
			"    i = i + 1;"
			"  end "
			"end");
		tra->commit(&db.status);

		tra = db.start();
		db.exec(tra, "create descending index doc_id_desc on doc (id)");
		db.exec(tra, "create index doc_upper_title on doc computed by (upper(title))");
		db.exec(tra, "create index doc_active on doc (status) where status = 'active'");
		db.exec(tra, "create index doc_num on doc (num)");
		tra->commit(&db.status);
		printf("3000 rows; indexes: descending, expression, partial, plain\n\n");

		tra = db.start();
		plan(db, tra, "select id from doc where upper(title) = 'TITLE 5'");
		plan(db, tra, "select id from doc where status = 'active'");
		plan(db, tra, "select first 1 id from doc order by id desc");
		plan(db, tra, "select id from doc where num = 42 or id = 7");
		plan(db, tra, "select id from doc where title containing 'itle 12'");

		printf("CONTAINING is correct but unindexed: matched %s rows by scanning all 3000\n",
			db.queryValue(tra,
				"select count(*) from doc where title containing 'itle 12'").c_str());
		tra->commit(&db.status);

		db.att->detach(&db.status);
		db.att = nullptr;
		printf("done.\n");
		return 0;
	}
	catch (const FbException& error)
	{
		return report(error);
	}
}
