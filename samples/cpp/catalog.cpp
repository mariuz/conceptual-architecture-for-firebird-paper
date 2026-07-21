/*
 *  catalog.cpp — companion to ../../catalog-bootstrap.md
 *
 *  The catalog describing itself, from client SQL.  On a freshly created
 *  database it shows:
 *    1. the fixed relation ids burned into relations.h declaration order
 *       (RDB$PAGES 0, RDB$DATABASE 1, RDB$FIELDS 2, RDB$RELATIONS 6);
 *    2. RDB$PAGES carrying its own pointer page — and the hdr_PAGES word
 *       on page 0 agreeing with it (the anti-recursion anchor);
 *    3. RDB$FORMATS empty while sixty-odd system relations with hundreds
 *       of columns are fully usable: their formats are compiled into
 *       libEngine (INI_init), not stored;
 *    4. a user table gaining RDB$FORMATS rows the moment DDL creates and
 *       alters it — user formats live in the catalog.
 */

#include "fb_sample.h"
#include <cstdint>

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost//tmp/fbhandson/catalog.fdb");
	const char* localFile = argOrDefault(argc, argv, 2, "/tmp/fbhandson/catalog.fdb");

	try
	{
		// A truly fresh database each run: drop it if it exists, recreate.
		{
			Db old;
			try { old.attach(database); } catch (const FbException&) {}
			if (old.att)
			{
				old.att->dropDatabase(&old.status);
				old.att = nullptr;
			}
		}
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();

		printf("-- 1. fixed relation ids (relations.h declaration order) --\n");
		Db::Table ids = db.query(tra,
			"select rdb$relation_id, trim(rdb$relation_name) "
			"from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1");
		ids.names = { "ID", "NAME" };			// friendlier than TRIM(...)'s default header
		Db::print(ids);

		printf("\n-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --\n");
		Db::print(db.query(tra,
			"select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type "
			"from rdb$pages where rdb$relation_id in (0, 6) "
			"order by rdb$relation_id, rdb$page_type, rdb$page_number"));

		// The anchor that cuts the recursion: hdr_PAGES at byte 28 of page 0.
		if (FILE* f = fopen(localFile, "rb"))
		{
			unsigned char b[4];
			uint32_t hdrPages = 0;
			if (fseek(f, 28, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4)
				memcpy(&hdrPages, b, 4);
			fclose(f);
			printf("\nhdr_PAGES (page 0, offset 28) = %u"
				"  <- matches the (relation 0, type 4) row above\n", hdrPages);
		}

		printf("\n-- 3. formats as code: zero stored formats, yet a full catalog --\n");
		Db::Table counts = db.query(tra,
			"select (select count(*) from rdb$formats), "
			"       (select count(*) from rdb$relations where rdb$system_flag = 1), "
			"       (select count(*) from rdb$relation_fields r join rdb$relations rel "
			"          on r.rdb$relation_name = rel.rdb$relation_name "
			"          and r.rdb$schema_name = rel.rdb$schema_name "
			"        where rel.rdb$system_flag = 1) "
			"from rdb$database");
		counts.names = { "FORMATS_ROWS", "SYS_RELATIONS", "SYS_FIELDS" };
		Db::print(counts);
		tra->commit(&db.status);

		printf("\n-- 4. user DDL writes formats into the catalog --\n");
		tra = db.start();
		db.exec(tra, "create table t1 (a integer)");
		tra->commit(&db.status);
		tra = db.start();
		db.exec(tra, "alter table t1 add b varchar(10)");
		tra->commit(&db.status);

		tra = db.start();
		Db::Table formats = db.query(tra,
			"select rdb$relation_id, rdb$format, octet_length(rdb$descriptor) "
			"from rdb$formats order by rdb$relation_id, rdb$format");
		formats.names = { "RDB$RELATION_ID", "RDB$FORMAT", "DESCRIPTOR_BYTES" };
		Db::print(formats);
		printf("\n(relation id of T1: %s — the first user id; "
			"system tables still contribute no rows)\n",
			db.queryValue(tra, "select rdb$relation_id from rdb$relations "
				"where rdb$relation_name = 'T1'").c_str());
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
