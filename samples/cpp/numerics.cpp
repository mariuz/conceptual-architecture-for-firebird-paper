/*
 *  numerics.cpp — companion sample for numeric-and-precision-arithmetic.md.
 *
 *  Four small experiments against a live server:
 *
 *   1. (0.1 + 0.2) - 0.3 computed in DOUBLE PRECISION vs DECFLOAT(34):
 *      binary floating point leaves a residue, decimal floating point is 0.
 *   2. NUMERIC(18,4) fetched RAW — the untouched output metadata reports
 *      SQL_INT64 with scale -4, and the message buffer contains the scaled
 *      integer 123456789 (the document's "scaled integer" claim, in bytes).
 *   3. INT128 at 2^127-1, and the overflow error one step beyond it.
 *   4. The DECFLOAT Division_by_zero trap (on by default), then cleared
 *      with SET DECFLOAT TRAPS TO so the same query returns Infinity.
 *
 *  Build/run: see ../../numeric-and-precision-arithmetic.md (Hands-on).
 */

#include "fb_sample.h"
#include <cinttypes>

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/numerics.fdb");
	try
	{
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();

		// -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3.
		printf("(0.1+0.2)-0.3 in DOUBLE PRECISION : %s\n",
			db.queryValue(tra,
				"SELECT (CAST(0.1 AS DOUBLE PRECISION) + 0.2) - 0.3 FROM RDB$DATABASE").c_str());
		printf("(0.1+0.2)-0.3 in DECFLOAT(34)     : %s\n\n",
			db.queryValue(tra,
				"SELECT (CAST(0.1 AS DECFLOAT(34)) + 0.2) - 0.3 FROM RDB$DATABASE").c_str());

		// -- 2. NUMERIC(18,4) on the wire: scaled integer + scale in the metadata.
		IStatement* stmt = db.att->prepare(&db.status, tra, 0,
			"SELECT CAST(12345.6789 AS NUMERIC(18,4)) FROM RDB$DATABASE",
			SQL_DIALECT_V6, IStatement::PREPARE_PREFETCH_METADATA);
		IMessageMetadata* meta = stmt->getOutputMetadata(&db.status);
		const unsigned type = meta->getType(&db.status, 0) & ~1u;
		const int scale = meta->getScale(&db.status, 0);
		const unsigned length = meta->getLength(&db.status, 0);
		printf("NUMERIC(18,4) wire format: type=%u (%s), length=%u, scale=%d\n",
			type, type == SQL_INT64 ? "SQL_INT64" : "?", length, scale);

		std::vector<unsigned char> buf(meta->getMessageLength(&db.status));
		IResultSet* curs = stmt->openCursor(&db.status, tra, nullptr, nullptr, meta, 0);
		curs->fetchNext(&db.status, buf.data());
		const unsigned char* p = buf.data() + meta->getOffset(&db.status, 0);
		int64_t raw;
		memcpy(&raw, p, sizeof raw);
		printf("message bytes (little-endian)  : ");
		for (unsigned i = 0; i < length; ++i)
			printf("%02x ", p[i]);
		printf("\nraw integer                    : %" PRId64 "\n", raw);
		printf("value = raw * 10^scale         : %" PRId64 " * 10^%d = %.4f\n\n",
			raw, scale, double(raw) * 1e-4);
		curs->close(&db.status);
		meta->release();
		stmt->free(&db.status);

		// -- 3. INT128: the full range, and one step past it.
		printf("INT128 max  : %s\n", db.queryValue(tra,
			"SELECT CAST(170141183460469231731687303715884105727 AS INT128) FROM RDB$DATABASE").c_str());
		try
		{
			db.queryValue(tra,
				"SELECT CAST(170141183460469231731687303715884105727 AS INT128) + 1 FROM RDB$DATABASE");
			printf("BUG: overflow not detected\n");
		}
		catch (const FbException& e)
		{
			char msg[256];
			db.util->formatStatus(msg, sizeof msg, e.getStatus());
			printf("INT128 max+1: %.100s\n\n", msg);
		}

		// -- 4. DECFLOAT division by zero: trapped by default, Infinity if untrapped.
		try
		{
			db.queryValue(tra,
				"SELECT CAST(1 AS DECFLOAT(16)) / 0 FROM RDB$DATABASE");
			printf("BUG: default trap did not fire\n");
		}
		catch (const FbException& e)
		{
			char msg[256];
			db.util->formatStatus(msg, sizeof msg, e.getStatus());
			printf("1/0 with default traps : %.60s\n", msg);
		}
		db.exec(tra, "SET DECFLOAT TRAPS TO");		// clear all traps
		printf("1/0 with traps cleared : %s\n", db.queryValue(tra,
			"SELECT CAST(1 AS DECFLOAT(16)) / 0 FROM RDB$DATABASE").c_str());

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
