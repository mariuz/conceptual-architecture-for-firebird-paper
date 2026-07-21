/*
 *  profiler.cpp (fb-cpp) — the accumulation view, driven from client code.
 *
 *  The fb-cpp twin of ../cpp/profiler.cpp: one RDB$PROFILER session
 *  brackets the same two workloads — a self-join over 5,000 rows and a
 *  20,000-iteration PSQL loop — then the PLG$PROFILER schema is read
 *  back like any other data.  The control surface is a SQL package and
 *  the output a SQL schema, so nothing is lost in the wrapper; what
 *  changes is only the fetch idiom (Statement loops, std::optional).
 *  The same pitfall applies unchanged: the plugin flushes through an
 *  autonomous transaction, so a RETAINED snapshot sees none of it —
 *  hard-commit and start a genuinely new Transaction before reading
 *  the views.  See ../../profiler.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_profiler [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/profiler_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		Transaction tra{att};

		// --- workload fixtures: a table and a looping procedure ----------
		try { att.execute(tra, "drop procedure hotspot"); } catch (const DatabaseException&) {}
		try { att.execute(tra, "drop table nums"); } catch (const DatabaseException&) {}
		att.execute(tra, "create table nums (id int primary key, val int)");
		tra.commitRetaining();
		att.execute(tra,
			"execute block as declare n int = 0; begin "
			"  while (n < 5000) do begin "
			"    insert into nums values (:n, mod(:n, 97)); n = n + 1; end end");
		att.execute(tra,
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
		tra.commitRetaining();

		// --- profile: START_SESSION ... workload ... FINISH_SESSION ------
		const std::int64_t profileId = att.queryScalar<std::int64_t>(tra,
			"select rdb$profiler.start_session('fb-cpp hands-on') from rdb$database")
			.value_or(-1);

		att.queryScalar<std::int64_t>(tra,
			"select count(*) from nums a join nums b on b.id = a.val");
		att.queryScalar<std::int64_t>(tra, "select total from hotspot");

		att.execute(tra, "execute procedure rdb$profiler.finish_session(true)");
		// The plugin flushes through an autonomous transaction; a retained
		// SNAPSHOT would not see it, so start a genuinely new transaction.
		tra.commit();
		Transaction read{att};
		printf("profile session %lld finished and flushed\n\n",
			static_cast<long long>(profileId));

		// --- the plan tree, with per-operator counters and times ---------
		printf("record sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):\n");
		printf("%-58s %5s %8s %13s\n", "ACCESS_PATH", "OPENS", "FETCHES", "TOTAL_NS");
		Statement rs{att, read,
			"select lpad('', level * 2) || cast(access_path as varchar(120)), "
			"       open_counter, fetch_counter, "
			"       open_fetch_total_elapsed_time "
			"from plg$profiler.plg$prof_record_source_stats_view "
			"where profile_id = ? and sql_text containing 'join nums' "
			"order by cursor_id, record_source_id"};
		rs.setInt64(0, profileId);
		for (bool row = rs.execute(read); row; row = rs.fetchNext())
			printf("%-58s %5lld %8lld %13lld\n",
				rs.getString(0).value_or("").c_str(),
				static_cast<long long>(rs.getInt64(1).value_or(0)),
				static_cast<long long>(rs.getInt64(2).value_or(0)),
				static_cast<long long>(rs.getInt64(3).value_or(0)));

		// --- the PSQL hotspot, per line and column -----------------------
		printf("\nhotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):\n");
		printf("%4s %6s %7s %12s %8s\n", "LINE", "COLUMN", "COUNTER", "TOTAL_NS", "AVG_NS");
		Statement psql{att, read,
			"select line_num, column_num, counter, "
			"       total_elapsed_time, avg_elapsed_time "
			"from plg$profiler.plg$prof_psql_stats_view "
			"where profile_id = ? and routine_name = 'HOTSPOT' "
			"order by total_elapsed_time desc"};
		psql.setInt64(0, profileId);
		for (bool row = psql.execute(read); row; row = psql.fetchNext())
			printf("%4d %6d %7lld %12lld %8lld\n",
				psql.getInt32(0).value_or(-1),
				psql.getInt32(1).value_or(-1),
				static_cast<long long>(psql.getInt64(2).value_or(0)),
				static_cast<long long>(psql.getInt64(3).value_or(0)),
				static_cast<long long>(psql.getInt64(4).value_or(0)));

		read.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
