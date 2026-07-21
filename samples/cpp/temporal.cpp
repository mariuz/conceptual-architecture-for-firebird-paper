/*
 *  temporal.cpp — companion sample for temporal-and-time-zones.md.
 *
 *  Shows the storage model the document describes, from the client side:
 *
 *   1. A raw fetch of TIMESTAMP '2026-07-18 12:00:00 America/New_York'
 *      exposes the wire struct ISC_TIMESTAMP_TZ — a UTC instant plus a
 *      2-byte zone id — and IUtil::decodeTimeStampTz turns the id back
 *      into the *named* zone.  The instant moved to 16:00 UTC; the zone
 *      survived: Firebird remembers where the value was, not just when.
 *   2. Named zone vs bare offset: the id decodes differently.
 *   3. AT TIME ZONE across a DST boundary: the same New York wall time
 *      lands differently in London in January (EST) and July (EDT).
 *   4. SET TIME ZONE: the session zone changes what CURRENT_TIMESTAMP
 *      and zoneless conversions mean.
 *
 *  Build/run: see ../../temporal-and-time-zones.md (Hands-on section).
 */

#include "fb_sample.h"

using namespace fbsample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/temporal.fdb");
	try
	{
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();

		// -- 1./2. Raw wire format of two literals: named zone vs offset.
		const char* sql =
			"SELECT TIMESTAMP '2026-07-18 12:00:00 America/New_York',"
			"       TIMESTAMP '2026-07-18 12:00:00 -05:00' FROM RDB$DATABASE";
		IStatement* stmt = db.att->prepare(&db.status, tra, 0, sql,
			SQL_DIALECT_V6, IStatement::PREPARE_PREFETCH_METADATA);
		IMessageMetadata* meta = stmt->getOutputMetadata(&db.status);
		std::vector<unsigned char> buf(meta->getMessageLength(&db.status));
		IResultSet* curs = stmt->openCursor(&db.status, tra, nullptr, nullptr, meta, 0);
		curs->fetchNext(&db.status, buf.data());

		for (unsigned i = 0; i < 2; ++i)
		{
			ISC_TIMESTAMP_TZ tz;
			memcpy(&tz, buf.data() + meta->getOffset(&db.status, i), sizeof tz);
			unsigned y, mo, d, h, mi, s, f;
			char zone[64];
			db.util->decodeTimeStampTz(&db.status, &tz,
				&y, &mo, &d, &h, &mi, &s, &f, sizeof zone, zone);
			printf("%s literal:\n", i == 0 ? "named-zone" : "offset");
			printf("  on the wire : UTC days=%d time=%u  zone id=%u\n",
				tz.utc_timestamp.timestamp_date, tz.utc_timestamp.timestamp_time,
				tz.time_zone);
			printf("  decoded     : %04u-%02u-%02u %02u:%02u:%02u %s\n",
				y, mo, d, h, mi, s, zone);
		}
		curs->close(&db.status);
		meta->release();
		stmt->free(&db.status);

		// -- 3. The same wall time across a DST boundary: New York noon is
		//       17:00 UTC in winter (EST) but 16:00 UTC in summer (EDT).
		printf("\nNY 12:00 in UTC, winter: %s\n", db.queryValue(tra,
			"SELECT TIMESTAMP '2026-01-18 12:00:00 America/New_York'"
			" AT TIME ZONE 'Etc/UTC' FROM RDB$DATABASE").c_str());
		printf("NY 12:00 in UTC, summer: %s\n", db.queryValue(tra,
			"SELECT TIMESTAMP '2026-07-18 12:00:00 America/New_York'"
			" AT TIME ZONE 'Etc/UTC' FROM RDB$DATABASE").c_str());

		// Equality is by UTC instant, regardless of zone spelling.
		printf("10:00 -02:00 = 09:00 -03:00 ? %s\n", db.queryValue(tra,
			"SELECT IIF(TIME '10:00:00 -02:00' = TIME '09:00:00 -03:00',"
			" 'EQUAL', 'different') FROM RDB$DATABASE").c_str());

		// -- 4. The session time zone governs "now" and zoneless conversions.
		printf("\nsession zone: %s   CURRENT_TIMESTAMP: %s\n",
			db.queryValue(tra,
				"SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE").c_str(),
			db.queryValue(tra,
				"SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE").c_str());
		db.exec(tra, "SET TIME ZONE 'Asia/Tokyo'");
		printf("session zone: %s     CURRENT_TIMESTAMP: %s\n",
			db.queryValue(tra,
				"SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE").c_str(),
			db.queryValue(tra,
				"SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE").c_str());

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
