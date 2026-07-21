/*
 *  extensibility.cpp — calling native code through SQL: UDR end to end.
 *
 *  Companion sample for ../../extensibility.md.  The shipped example UDR
 *  module (plugins/udr/libudrcpp_example.so, source in
 *  extern/firebird/examples/udr/) is bound to SQL objects with
 *  EXTERNAL NAME '<module>!<entry>' ENGINE udr, then called like any other
 *  procedure/function — the whole extension path of the document's Figure 2:
 *  engine -> udr_engine plugin (TYPE_EXTERNAL_ENGINE) -> native module.
 *
 *  The sample also shows the SQL-visible surface of the plugin architecture:
 *  RDB$CONFIG names the plugin filling each role, and RDB$PROCEDURES /
 *  RDB$FUNCTIONS record the module!entry binding as ordinary metadata.
 */

#include "fb_sample.h"

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/extensibility.fdb");

	try
	{
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();

		// 1. Bind SQL names to entry points in the shipped native module.
		db.exec(tra,
			"recreate procedure gen_rows (start_n integer not null, "
			"                             end_n integer not null) "
			"  returns (n integer not null) "
			"  external name 'udrcpp_example!gen_rows' engine udr");
		db.exec(tra,
			"recreate function sum_args (n1 integer, n2 integer, n3 integer) "
			"  returns integer "
			"  external name 'udrcpp_example!sum_args' engine udr");
		tra->commitRetaining(&db.status);

		// 2. Call them: native C++ running inside the server, plain SQL here.
		printf("select n from gen_rows(1, 5):\n");
		Db::print(db.query(tra, "select n from gen_rows(1, 5)"));

		printf("\nselect sum_args(19, 20, 3):  %s\n",
			db.queryValue(tra,
				"select sum_args(19, 20, 3) from rdb$database").c_str());

		// 3. The binding is ordinary metadata...
		printf("\nexternal routines recorded in the system tables:\n");
		Db::print(db.query(tra,
			"select trim(rdb$procedure_name) || '  ->  ' || "
			"       trim(rdb$entrypoint) || '  (engine ' || "
			"       trim(rdb$engine_name) || ')' "
			"from rdb$procedures where rdb$engine_name = 'UDR' "
			"union all "
			"select trim(rdb$function_name) || '  ->  ' || "
			"       trim(rdb$entrypoint) || '  (engine ' || "
			"       trim(rdb$engine_name) || ')' "
			"from rdb$functions where rdb$engine_name = 'UDR'"));

		// 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG.
		printf("\nplugins filling each role (rdb$config):\n");
		Db::print(db.query(tra,
			"select rdb$config_name role, rdb$config_value plugin "
			"from rdb$config "
			"where rdb$config_name in ('Providers', 'AuthServer', "
			"      'UserManager', 'WireCryptPlugin', 'TracePlugin', "
			"      'DefaultProfilerPlugin') "
			"order by rdb$config_id"));

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
