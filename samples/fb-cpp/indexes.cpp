/*
 *  indexes.cpp (fb-cpp) — companion to ../../indexing-and-full-text-search.md
 *
 *  The fb-cpp twin of ../cpp/indexes.cpp: the same 3,000-row table, the
 *  same four index variants, the same five plans.  Where the OO API calls
 *  IStatement::getPlan(status, false) after the fact, fb-cpp asks for the
 *  plan at prepare time — StatementOptions().setPrefetchLegacyPlan(true)
 *  — and hands it back as a std::string from getLegacyPlan(); there is a
 *  getPlan() sibling for the structured "explain" form the document's
 *  isql sessions show with SET EXPLAIN.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_indexes [database]
 */

#include "fbcpp_sample.h"
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static void plan(Attachment& att, Transaction& tra, const char* sql)
{
	Statement stmt{att, tra, sql, StatementOptions().setPrefetchLegacyPlan(true)};
	std::string p = stmt.getLegacyPlan();
	p.erase(0, p.find_first_not_of('\n'));		// the server prefixes one newline
	printf("%s\n%s\n\n", sql, p.empty() ? "(no plan)" : p.c_str());
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/indexes_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		{
			Transaction t{att};
			att.execute(t, "recreate table doc ("
				" id integer, title varchar(60), status varchar(10), num integer)");
			t.commit();
		}
		{
			Transaction t{att};
			att.execute(t,
				"execute block as declare i integer = 0; begin"
				"  while (i < 3000) do begin"
				"    insert into doc values (:i, 'Title ' || :i,"
				"      iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100));"
				"    i = i + 1;"
				"  end "
				"end");
			t.commit();
		}
		{
			Transaction t{att};
			att.execute(t, "create descending index doc_id_desc on doc (id)");
			att.execute(t, "create index doc_upper_title on doc computed by (upper(title))");
			att.execute(t, "create index doc_active on doc (status) where status = 'active'");
			att.execute(t, "create index doc_num on doc (num)");
			t.commit();
		}
		printf("3000 rows; indexes: descending, expression, partial, plain\n\n");

		Transaction tra{att};
		plan(att, tra, "select id from doc where upper(title) = 'TITLE 5'");
		plan(att, tra, "select id from doc where status = 'active'");
		plan(att, tra, "select first 1 id from doc order by id desc");
		plan(att, tra, "select id from doc where num = 42 or id = 7");
		plan(att, tra, "select id from doc where title containing 'itle 12'");

		Statement count{att, tra,
			"select count(*) from doc where title containing 'itle 12'"};
		count.execute(tra);
		printf("CONTAINING is correct but unindexed: matched %lld rows by scanning all 3000\n",
			(long long) count.getInt64(0).value_or(0));
		tra.commit();

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
