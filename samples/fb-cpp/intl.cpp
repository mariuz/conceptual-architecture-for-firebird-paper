/*
 *  intl.cpp (fb-cpp) — collations, per-column charsets, and transliteration.
 *
 *  The fb-cpp twin of ../cpp/intl.cpp: the same three demonstrations
 *  (UNICODE_CI_AI vs UCS_BASIC equality, per-column charsets, and the
 *  connection-charset transliteration hex-dump).  The instructive diff is
 *  that the OO-API sample needed a special rawFetch() helper to bypass its
 *  own CS_NONE-coercing query helper; fb-cpp always fetches through the
 *  statement's own output metadata, so getString() hands over exactly the
 *  bytes the engine transliterated into the CONNECTION charset — the effect
 *  this sample demonstrates, with no workaround.  The connection charset
 *  itself is one typed builder call: setConnectionCharSet("UTF8"/"NONE")
 *  (fb-cpp sets no lc_ctype at all unless asked).
 *  See ../../internationalization.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_intl [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static void exec(Attachment& att, Transaction& tra, const std::string& sql)
{
	Statement{att, tra, sql}.execute(tra);
}

static std::string one(Attachment& att, Transaction& tra, const char* sql)
{
	Statement stmt{att, tra, sql};
	stmt.execute(tra);
	return stmt.getString(0).value_or("<null>");
}

static void hexdump(const char* label, const std::string& v)
{
	printf("  %-16s len=%2zu  ", label, v.size());
	for (unsigned char c : v)
		printf("%02X ", c);
	printf("  \"%s\"\n", v.c_str());
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/intl_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};

		// The main attachment speaks UTF8 — the engine transliterates every
		// text column into it on the way out.
		Attachment att = [&]
		{
			try
			{
				return Attachment{client, database,
					defaultOptions().setConnectionCharSet("UTF8")};
			}
			catch (const FbCppException&)
			{
				return Attachment{client, database, defaultOptions()
					.setConnectionCharSet("UTF8").setCreateDatabase(true)};
			}
		}();

		{
			Transaction ddl{att};
			try { exec(att, ddl, "DROP TABLE t"); } catch (const DatabaseException&) {}
			exec(att, ddl,
				"CREATE TABLE t ("
				"  name_ci_ai VARCHAR(30) CHARACTER SET UTF8 COLLATE UNICODE_CI_AI,"
				"  name_bin   VARCHAR(30) CHARACTER SET UTF8 COLLATE UCS_BASIC,"
				"  name_win   VARCHAR(30) CHARACTER SET WIN1252)");
			ddl.commit();
		}

		Transaction tra{att};
		for (const char* v : { "Café", "CAFE", "cafe" })
			exec(att, tra, std::string("INSERT INTO t VALUES ('") + v + "','" + v + "','" + v + "')");

		// -- 1. The collation, not the data, decides what "equal" means.
		printf("rows matching 'cafe' with UNICODE_CI_AI : %s\n",
			one(att, tra, "SELECT COUNT(*) FROM t WHERE name_ci_ai = 'cafe'").c_str());
		printf("rows matching 'cafe' with UCS_BASIC     : %s\n",
			one(att, tra, "SELECT COUNT(*) FROM t WHERE name_bin = 'cafe'").c_str());
		printf("UPPER('café èñ ß')                      : %s\n\n",
			one(att, tra, "SELECT UPPER('café èñ ß') FROM RDB$DATABASE").c_str());

		// Sorting differs too: CI_AI groups the spellings, UCS_BASIC is binary.
		printf("ORDER BY name_ci_ai: ");
		{
			Statement s{att, tra, "SELECT name_ci_ai FROM t ORDER BY name_ci_ai"};
			for (bool ok = s.execute(tra); ok; ok = s.fetchNext())
				printf("%s  ", s.getString(0)->c_str());
		}
		printf("\nORDER BY name_bin  : ");
		{
			Statement s{att, tra, "SELECT name_bin FROM t ORDER BY name_bin"};
			for (bool ok = s.execute(tra); ok; ok = s.fetchNext())
				printf("%s  ", s.getString(0)->c_str());
		}
		printf("   (binary: uppercase codepoints first)\n\n");
		tra.commit();

		// -- 2./3. Same stored WIN1252 'Café', two connection charsets.
		Attachment none{client, database,
			defaultOptions().setConnectionCharSet("NONE")};

		printf("SELECT name_win FROM t WHERE name_bin = 'Café' — same row, two connections:\n");
		{
			Transaction t1{att};
			hexdump("lc_ctype=UTF8:",
				one(att, t1, "SELECT name_win FROM t WHERE name_bin = 'Café'"));
			t1.commit();
		}
		{
			Transaction t2{none};
			hexdump("lc_ctype=NONE:",
				one(none, t2, "SELECT name_win FROM t WHERE name_bin = 'Caf\xc3\xa9'"));
			t2.commit();
		}
		printf("  -> the column stores E9 (WIN1252); the UTF8 connection receives the\n"
			"     transliterated C3 A9, the NONE connection the raw stored byte.\n");

		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
