/*
 *  temporal.cpp (fb-cpp) — the two faces of TIMESTAMP WITH TIME ZONE.
 *
 *  The fb-cpp twin of ../cpp/temporal.cpp: the same storage-model tour.
 *  The OO-API sample had to fetch the raw message buffer, memcpy an
 *  ISC_TIMESTAMP_TZ out of it and call IUtil::decodeTimeStampTz by hand;
 *  fb-cpp exposes BOTH faces as typed getters on the same column —
 *  getOpaqueTimestampTz() is the wire struct (UTC instant + 2-byte zone
 *  id) and getTimestampTz() is the decoded {std::chrono UTC timestamp,
 *  zone NAME}.  The DST-boundary, instant-equality and SET TIME ZONE
 *  demonstrations are pure SQL and port unchanged.
 *  See ../../temporal-and-time-zones.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_temporal [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static std::string one(Attachment& att, Transaction& tra, const char* sql)
{
	Statement stmt{att, tra, sql};
	stmt.execute(tra);
	return stmt.getString(0).value_or("<null>");
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/temporal_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		Transaction tra{att};

		// -- 1./2. Both faces of two literals: named zone vs offset.
		Statement s{att, tra,
			"SELECT TIMESTAMP '2026-07-18 12:00:00 America/New_York',"
			"       TIMESTAMP '2026-07-18 12:00:00 -05:00' FROM RDB$DATABASE"};
		s.execute(tra);
		for (unsigned i = 0; i < 2; ++i)
		{
			const OpaqueTimestampTz wire = *s.getOpaqueTimestampTz(i);
			const TimestampTz decoded = *s.getTimestampTz(i);
			printf("%s literal:\n", i == 0 ? "named-zone" : "offset");
			printf("  on the wire : UTC days=%d time=%u  zone id=%u\n",
				wire.value.utc_timestamp.timestamp_date,
				wire.value.utc_timestamp.timestamp_time,
				wire.value.time_zone);
			printf("  decoded     : %04d-%02u-%02u %02ld:%02ld UTC, zone \"%s\"\n",
				static_cast<int>(decoded.utcTimestamp.date.year()),
				static_cast<unsigned>(decoded.utcTimestamp.date.month()),
				static_cast<unsigned>(decoded.utcTimestamp.date.day()),
				static_cast<long>(decoded.utcTimestamp.time.hours().count()),
				static_cast<long>(decoded.utcTimestamp.time.minutes().count()),
				decoded.zone.c_str());
		}

		// -- 3. The same wall time across a DST boundary: New York noon is
		//       17:00 UTC in winter (EST) but 16:00 UTC in summer (EDT).
		printf("\nNY 12:00 in UTC, winter: %s\n", one(att, tra,
			"SELECT TIMESTAMP '2026-01-18 12:00:00 America/New_York'"
			" AT TIME ZONE 'Etc/UTC' FROM RDB$DATABASE").c_str());
		printf("NY 12:00 in UTC, summer: %s\n", one(att, tra,
			"SELECT TIMESTAMP '2026-07-18 12:00:00 America/New_York'"
			" AT TIME ZONE 'Etc/UTC' FROM RDB$DATABASE").c_str());

		// Equality is by UTC instant, regardless of zone spelling.
		printf("10:00 -02:00 = 09:00 -03:00 ? %s\n", one(att, tra,
			"SELECT IIF(TIME '10:00:00 -02:00' = TIME '09:00:00 -03:00',"
			" 'EQUAL', 'different') FROM RDB$DATABASE").c_str());

		// -- 4. The session time zone governs "now" and zoneless conversions.
		printf("\nsession zone: %s   CURRENT_TIMESTAMP: %s\n",
			one(att, tra,
				"SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE").c_str(),
			one(att, tra,
				"SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE").c_str());
		Statement{att, tra, "SET TIME ZONE 'Asia/Tokyo'"}.execute(tra);
		printf("session zone: %s     CURRENT_TIMESTAMP: %s\n",
			one(att, tra,
				"SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE").c_str(),
			one(att, tra,
				"SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE").c_str());

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
