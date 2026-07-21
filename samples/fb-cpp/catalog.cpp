/*
 *  catalog.cpp (fb-cpp) — companion to ../../catalog-bootstrap.md
 *
 *  The fb-cpp twin of ../cpp/catalog.cpp: the same four self-description
 *  queries against a freshly recreated database — fixed relation ids,
 *  RDB$PAGES carrying its own pointer page (cross-checked against the raw
 *  hdr_PAGES word on page 0), RDB$FORMATS empty for the compiled-in
 *  system relations, then user DDL planting the first stored formats.
 *  fb-cpp's contribution is the plumbing: drop-and-recreate is
 *  Attachment::dropDatabase() plus setCreateDatabase(true), DDL is
 *  Attachment::execute(), and every value fetches as std::optional.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_catalog [database] [localFile]
 */

#include "fbcpp_sample.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace fbcpp;
using namespace fbcpp_sample;

// Run a query and print it as an aligned table (the fb_sample.h Db::print
// of this twin), fetching every column through getString's conversions.
static void printQuery(Attachment& att, Transaction& tra, const char* sql,
	std::vector<std::string> names)
{
	Statement stmt{att, tra, sql};
	std::vector<std::vector<std::string>> rows;
	for (bool more = stmt.execute(tra); more; more = stmt.fetchNext())
	{
		std::vector<std::string> row;
		for (unsigned i = 0; i < names.size(); ++i)
			row.push_back(stmt.getString(i).value_or("<null>"));
		rows.push_back(std::move(row));
	}

	std::vector<size_t> w;
	for (size_t c = 0; c < names.size(); ++c)
	{
		size_t m = names[c].size();
		for (const auto& r : rows)
			m = r[c].size() > m ? r[c].size() : m;
		w.push_back(m);
	}
	for (size_t c = 0; c < names.size(); ++c)
		printf("%-*s%s", (int) w[c], names[c].c_str(), c + 1 < names.size() ? " " : "\n");
	for (size_t c = 0; c < names.size(); ++c)
		printf("%s%s", std::string(w[c], '-').c_str(), c + 1 < names.size() ? " " : "\n");
	for (const auto& r : rows)
		for (size_t c = 0; c < names.size(); ++c)
			printf("%-*s%s", (int) w[c], r[c].c_str(), c + 1 < names.size() ? " " : "\n");
}

static std::string queryValue(Attachment& att, Transaction& tra, const char* sql)
{
	Statement stmt{att, tra, sql};
	stmt.execute(tra);
	return stmt.getString(0).value_or("<null>");
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/catalog_fbcpp.fdb");
	const char* localFile = argOrDefault(argc, argv, 2, "/tmp/fbhandson/catalog_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};

		// A truly fresh database each run: drop it if it exists, recreate.
		try
		{
			Attachment old{client, database, defaultOptions()};
			old.dropDatabase();
		}
		catch (const FbCppException&) {}
		Attachment att{client, database, defaultOptions().setCreateDatabase(true)};

		Transaction tra{att};
		printf("-- 1. fixed relation ids (relations.h declaration order) --\n");
		printQuery(att, tra,
			"select rdb$relation_id, trim(rdb$relation_name) "
			"from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1",
			{ "ID", "NAME" });

		printf("\n-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --\n");
		printQuery(att, tra,
			"select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type "
			"from rdb$pages where rdb$relation_id in (0, 6) "
			"order by rdb$relation_id, rdb$page_type, rdb$page_number",
			{ "RDB$PAGE_NUMBER", "RDB$RELATION_ID", "RDB$PAGE_SEQUENCE", "RDB$PAGE_TYPE" });

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
		printQuery(att, tra,
			"select (select count(*) from rdb$formats), "
			"       (select count(*) from rdb$relations where rdb$system_flag = 1), "
			"       (select count(*) from rdb$relation_fields r join rdb$relations rel "
			"          on r.rdb$relation_name = rel.rdb$relation_name "
			"          and r.rdb$schema_name = rel.rdb$schema_name "
			"        where rel.rdb$system_flag = 1) "
			"from rdb$database",
			{ "FORMATS_ROWS", "SYS_RELATIONS", "SYS_FIELDS" });
		tra.commit();

		printf("\n-- 4. user DDL writes formats into the catalog --\n");
		{
			Transaction t{att};
			att.execute(t, "create table t1 (a integer)");
			t.commit();
		}
		{
			Transaction t{att};
			att.execute(t, "alter table t1 add b varchar(10)");
			t.commit();
		}

		Transaction tra2{att};
		printQuery(att, tra2,
			"select rdb$relation_id, rdb$format, octet_length(rdb$descriptor) "
			"from rdb$formats order by rdb$relation_id, rdb$format",
			{ "RDB$RELATION_ID", "RDB$FORMAT", "DESCRIPTOR_BYTES" });
		printf("\n(relation id of T1: %s — the first user id; "
			"system tables still contribute no rows)\n",
			queryValue(att, tra2, "select rdb$relation_id from rdb$relations "
				"where rdb$relation_name = 'T1'").c_str());
		tra2.commit();

		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
