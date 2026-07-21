/*
 *  profiler.cpp — the accumulation view, driven from client code.
 *
 *  Companion sample for ../../profiler.md.  One profiler session brackets
 *  two workloads, then the PLG$PROFILER schema is queried like any other
 *  data — the document's argument that profiling output should be
 *  normalized tables, made runnable:
 *
 *    - a join, read back from PLG$PROF_RECORD_SOURCE_STATS_VIEW as an
 *      indented plan tree with per-operator open/fetch counts and times;
 *    - a PSQL procedure with a hot loop, read back per line and column
 *      from PLG$PROF_PSQL_STATS_VIEW.
 *
 *  Everything runs over a normal remote attachment: START_SESSION or
 *  FINISH_SESSION are just SQL, and the results are just rows.
 */

#include "fb_sample.h"

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/profiler.fdb");

	try
	{
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();

		// --- workload fixtures: a table and a looping procedure ----------
		try { db.exec(tra, "drop procedure hotspot"); } catch (const FbException&) {}
		try { db.exec(tra, "drop table nums"); } catch (const FbException&) {}
		db.exec(tra, "create table nums (id int primary key, val int)");
		tra->commitRetaining(&db.status);
		db.exec(tra,
			"execute block as declare n int = 0; begin "
			"  while (n < 5000) do begin "
			"    insert into nums values (:n, mod(:n, 97)); n = n + 1; end end");
		db.exec(tra,
			"create procedure hotspot returns (total bigint) as\n"
			"  declare i int = 0;\n"
			"  declare x int;\n"
			"begin\n"
			"  total = 0;\n"
			"  while (i < 20000) do\n"
			"  begin\n"
			"    select val from nums where id = mod(:i, 5000) into :x;\n"
			"    total = total + coalesce(:x, 0);\n"
			"    i = i + 1;\n"
			"  end\n"
			"  suspend;\n"
			"end");
		tra->commitRetaining(&db.status);

		// --- profile: START_SESSION ... workload ... FINISH_SESSION ------
		std::string profileId = db.queryValue(tra,
			"select rdb$profiler.start_session('cpp hands-on') from rdb$database");

		db.queryValue(tra,
			"select count(*) from nums a join nums b on b.id = a.val");
		db.queryValue(tra, "select total from hotspot");

		db.exec(tra, "execute procedure rdb$profiler.finish_session(true)");
		// The plugin flushes through an autonomous transaction; a retained
		// SNAPSHOT would not see it, so start a genuinely new transaction.
		tra->commit(&db.status);
		tra = db.start();
		printf("profile session %s finished and flushed\n\n", profileId.c_str());

		// --- the plan tree, with per-operator counters and times ---------
		printf("record sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):\n");
		Db::print(db.query(tra,
			"select cast(lpad('', level * 2) || cast(access_path as varchar(120)) "
			"           as varchar(140)) as access_path, "
			"       open_counter as opens, fetch_counter as fetches, "
			"       open_fetch_total_elapsed_time as total_ns "
			"from plg$profiler.plg$prof_record_source_stats_view "
			"where profile_id = " + profileId + " "
			"  and sql_text containing 'join nums' "
			"order by cursor_id, record_source_id"));

		// --- the PSQL hotspot, per line and column -----------------------
		printf("\nhotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):\n");
		Db::print(db.query(tra,
			"select line_num, column_num, counter, "
			"       total_elapsed_time as total_ns, avg_elapsed_time as avg_ns "
			"from plg$profiler.plg$prof_psql_stats_view "
			"where profile_id = " + profileId + " "
			"  and routine_name = 'HOTSPOT' "
			"order by total_elapsed_time desc"));

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
