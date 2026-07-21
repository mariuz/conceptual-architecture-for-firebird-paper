/*
 *  intl.cpp — companion sample for internationalization.md.
 *
 *  Three demonstrations on one small table:
 *
 *   1. Collation decides equality: the same three rows ('Café','CAFE',
 *      'cafe') match a query for 'cafe' 3 times under UNICODE_CI_AI and
 *      once under UCS_BASIC; UPPER() folds accented letters correctly.
 *   2. Per-column charsets: one column is UTF8, its neighbour WIN1252 —
 *      in the same table.
 *   3. Transliteration is driven by the CONNECTION charset (lc_ctype):
 *      the program attaches twice, once with lc_ctype=UTF8 and once with
 *      lc_ctype=NONE, reads the same WIN1252 'Café', and hex-dumps what
 *      arrives: the UTF8 connection gets é as C3 A9 (transliterated), the
 *      NONE connection gets the raw stored WIN1252 byte E9 (passthrough).
 *
 *  Build/run: see ../../internationalization.md (Hands-on section).
 */

#include "fb_sample.h"

using namespace fbsample;

static void hexdump(const char* label, const std::string& v)
{
	printf("  %-16s len=%2zu  ", label, v.size());
	for (unsigned char c : v)
		printf("%02X ", c);
	printf("  \"%s\"\n", v.c_str());
}

// Fetch one VARCHAR value with the statement's OWN output metadata — no
// coercion, so the engine transliterates the column charset into the
// connection charset (fb_sample.h's query() would force CS_NONE instead
// and hide exactly the effect this sample demonstrates).
static std::string rawFetch(Db& db, ITransaction* tra, const char* sql)
{
	IStatement* stmt = db.att->prepare(&db.status, tra, 0, sql,
		SQL_DIALECT_V6, IStatement::PREPARE_PREFETCH_METADATA);
	IMessageMetadata* meta = stmt->getOutputMetadata(&db.status);
	std::vector<unsigned char> buf(meta->getMessageLength(&db.status));
	IResultSet* curs = stmt->openCursor(&db.status, tra, nullptr, nullptr, nullptr, 0);
	curs->fetchNext(&db.status, buf.data());
	const unsigned char* p = buf.data() + meta->getOffset(&db.status, 0);
	unsigned short len;
	memcpy(&len, p, sizeof len);
	std::string out(reinterpret_cast<const char*>(p + 2), len);
	curs->close(&db.status);
	meta->release();
	stmt->free(&db.status);
	return out;
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/intl.fdb");
	try
	{
		Db db;						// connection charset UTF8 (helper default)
		db.attachOrCreate(database);

		ITransaction* ddl = db.start();
		try { db.exec(ddl, "DROP TABLE t"); } catch (const FbException&) {}
		db.exec(ddl,
			"CREATE TABLE t ("
			"  name_ci_ai VARCHAR(30) CHARACTER SET UTF8 COLLATE UNICODE_CI_AI,"
			"  name_bin   VARCHAR(30) CHARACTER SET UTF8 COLLATE UCS_BASIC,"
			"  name_win   VARCHAR(30) CHARACTER SET WIN1252)");
		ddl->commit(&db.status);

		ITransaction* tra = db.start();
		for (const char* v : { "Café", "CAFE", "cafe" })
			db.exec(tra, std::string("INSERT INTO t VALUES ('") + v + "','" + v + "','" + v + "')");

		// -- 1. The collation, not the data, decides what "equal" means.
		printf("rows matching 'cafe' with UNICODE_CI_AI : %s\n",
			db.queryValue(tra, "SELECT COUNT(*) FROM t WHERE name_ci_ai = 'cafe'").c_str());
		printf("rows matching 'cafe' with UCS_BASIC     : %s\n",
			db.queryValue(tra, "SELECT COUNT(*) FROM t WHERE name_bin = 'cafe'").c_str());
		printf("UPPER('café èñ ß')                      : %s\n\n",
			db.queryValue(tra, "SELECT UPPER('café èñ ß') FROM RDB$DATABASE").c_str());

		// Sorting differs too: CI_AI groups the spellings, UCS_BASIC is binary.
		printf("ORDER BY name_ci_ai: ");
		for (auto& r : db.query(tra, "SELECT name_ci_ai FROM t ORDER BY name_ci_ai").rows)
			printf("%s  ", r[0].c_str());
		printf("\nORDER BY name_bin  : ");
		for (auto& r : db.query(tra, "SELECT name_bin FROM t ORDER BY name_bin").rows)
			printf("%s  ", r[0].c_str());
		printf("   (binary: uppercase codepoints first)\n\n");
		tra->commit(&db.status);

		// -- 2./3. Same stored WIN1252 'Café', two connection charsets.
		Db none;
		none.attach(database, Db::defaultUser(), Db::defaultPassword(), "NONE");

		printf("SELECT name_win FROM t WHERE name_bin = 'Café' — same row, two connections:\n");
		{
			ITransaction* t1 = db.start();
			hexdump("lc_ctype=UTF8:",
				rawFetch(db, t1, "SELECT name_win FROM t WHERE name_bin = 'Café'"));
			t1->commit(&db.status);
		}
		{
			ITransaction* t2 = none.start();
			hexdump("lc_ctype=NONE:",
				rawFetch(none, t2, "SELECT name_win FROM t WHERE name_bin = 'Caf\xc3\xa9'"));
			t2->commit(&none.status);
		}
		printf("  -> the column stores E9 (WIN1252); the UTF8 connection receives the\n"
			"     transliterated C3 A9, the NONE connection the raw stored byte.\n");

		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
