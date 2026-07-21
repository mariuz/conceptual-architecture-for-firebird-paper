/*
 *  extensibility.cpp (fb-cpp) — calling native code through SQL: UDR end to end.
 *
 *  The fb-cpp twin of ../cpp/extensibility.cpp: bind the shipped example
 *  UDR module (plugins/udr/libudrcpp_example.so) to SQL names with
 *  EXTERNAL NAME '<module>!<entry>' ENGINE udr, call both routines, then
 *  read the binding back from RDB$PROCEDURES / RDB$FUNCTIONS and the
 *  plugin roster from RDB$CONFIG.  Everything here is plain SQL — the
 *  extension seams all live on the server side of the wire — so the
 *  mirror is nearly 1:1; what changes is only the client idiom:
 *  Attachment::execute / queryScalar / Statement fetch loops instead of
 *  hand-rolled metadata handling.  See ../../extensibility.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_extensibility [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/extensibility_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		Transaction tra{att};

		// 1. Bind SQL names to entry points in the shipped native module.
		att.execute(tra,
			"recreate procedure gen_rows (start_n integer not null, "
			"                             end_n integer not null) "
			"  returns (n integer not null) "
			"  external name 'udrcpp_example!gen_rows' engine udr");
		att.execute(tra,
			"recreate function sum_args (n1 integer, n2 integer, n3 integer) "
			"  returns integer "
			"  external name 'udrcpp_example!sum_args' engine udr");
		tra.commitRetaining();

		// 2. Call them: native C++ running inside the server, plain SQL here.
		printf("select n from gen_rows(1, 5):\n");
		Statement gen{att, tra, "select n from gen_rows(1, 5)"};
		for (bool row = gen.execute(tra); row; row = gen.fetchNext())
			printf("    n = %d\n", gen.getInt32(0).value_or(-1));

		printf("\nselect sum_args(19, 20, 3):  %d\n",
			att.queryScalar<std::int32_t>(tra,
				"select sum_args(19, 20, 3) from rdb$database").value_or(-1));

		// 3. The binding is ordinary metadata...
		printf("\nexternal routines recorded in the system tables:\n");
		Statement routines{att, tra,
			"select trim(rdb$procedure_name) || '  ->  ' || "
			"       trim(rdb$entrypoint) || '  (engine ' || "
			"       trim(rdb$engine_name) || ')' "
			"from rdb$procedures where rdb$engine_name = 'UDR' "
			"union all "
			"select trim(rdb$function_name) || '  ->  ' || "
			"       trim(rdb$entrypoint) || '  (engine ' || "
			"       trim(rdb$engine_name) || ')' "
			"from rdb$functions where rdb$engine_name = 'UDR'"};
		for (bool row = routines.execute(tra); row; row = routines.fetchNext())
			printf("    %s\n", routines.getString(0).value_or("").c_str());

		// 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG.
		printf("\nplugins filling each role (rdb$config):\n");
		Statement roles{att, tra,
			"select trim(rdb$config_name), trim(rdb$config_value) "
			"from rdb$config "
			"where rdb$config_name in ('Providers', 'AuthServer', "
			"      'UserManager', 'WireCryptPlugin', 'TracePlugin', "
			"      'DefaultProfilerPlugin') "
			"order by rdb$config_id"};
		for (bool row = roles.execute(tra); row; row = roles.fetchNext())
			printf("    %-22s %s\n",
				roles.getString(0).value_or("").c_str(),
				roles.getString(1).value_or("").c_str());

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
