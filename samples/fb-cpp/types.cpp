/*
 *  types.cpp (fb-cpp) — the headline types as native C++ values.
 *
 *  The fb-cpp twin of ../cpp/types.cpp: the same showcase table (BOOLEAN,
 *  INT128, DECFLOAT(34), TIMESTAMP WITH TIME ZONE, CHECK-constrained
 *  domain), but where the OO-API sample coerced every column to VARCHAR
 *  and let the server's CVT rules render text, this one fetches each
 *  column TYPED: getBool -> bool, getBoostInt128 -> a real 128-bit
 *  integer (Boost.Multiprecision), getBoostDecFloat34 -> a real 34-digit
 *  decimal, getTimestampTz -> {UTC instant, zone NAME}.  The wire types
 *  come from fb-cpp's cached Descriptors instead of raw IMessageMetadata
 *  calls.  See ../../sql-dialect-and-types.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_types [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <sstream>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* typeName(DescriptorOriginalType t)
{
	switch (t)
	{
	case DescriptorOriginalType::VARYING: return "VARYING (VARCHAR)";
	case DescriptorOriginalType::BOOLEAN: return "BOOLEAN";
	case DescriptorOriginalType::INT128: return "INT128";
	case DescriptorOriginalType::DEC34: return "DEC34 (DECFLOAT(34))";
	case DescriptorOriginalType::TIMESTAMP_TZ: return "TIMESTAMP_TZ";
	case DescriptorOriginalType::INT64: return "INT64 (BIGINT/NUMERIC)";
	default: return "(other)";
	}
}

static std::string firstLine(const char* what)
{
	const std::string s{what};
	return s.substr(0, s.find('\n'));
}

template <typename T>
static std::string str(const T& v)
{
	std::ostringstream os;
	os << v;
	return os.str();
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/types_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		// Idempotent cleanup — each drop commits alone (DDL is partly
		// deferred to commit, so the domain's dependency check needs the
		// table drop already committed).
		for (const char* sql : { "DROP TABLE showcase", "DROP DOMAIN d_email" })
		{
			Transaction t{att};
			try
			{
				Statement{att, t, sql}.execute(t);
				t.commit();
			}
			catch (const DatabaseException&)
			{
				t.rollback();
			}
		}

		{
			Transaction ddl{att};
			Statement{att, ddl,
				"CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')"}.execute(ddl);
			Statement{att, ddl,
				"CREATE TABLE showcase ("
				"  flag  BOOLEAN,"
				"  big   INT128,"
				"  money DECFLOAT(34),"
				"  born  TIMESTAMP WITH TIME ZONE,"
				"  mail  d_email)"}.execute(ddl);
			ddl.commit();
		}

		Transaction tra{att};
		Statement{att, tra,
			"INSERT INTO showcase VALUES ("
			"  TRUE,"
			"  170141183460469231731687303715884105727,"		// INT128 max
			"  0.1,"											// exact in DECFLOAT
			"  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"	// named zone
			"  'user@example.com')"}.execute(tra);

		// The domain's CHECK travels with the type: a mail without '@' dies.
		try
		{
			Statement{att, tra, "INSERT INTO showcase (mail) VALUES ('not-an-address')"}
				.execute(tra);
			printf("BUG: domain CHECK did not fire\n");
		}
		catch (const DatabaseException& e)
		{
			printf("domain CHECK rejected 'not-an-address' (gds %ld):\n    %s\n\n",
				static_cast<long>(e.getErrorCode()), firstLine(e.what()).c_str());
		}

		// -- What the wire carries: fb-cpp caches one Descriptor per column.
		Statement sel{att, tra, "SELECT * FROM showcase"};
		printf("column  wire type (Descriptor.originalType)\n");
		printf("------  -----------------------------------\n");
		for (const auto& d : sel.getOutputDescriptors())
			printf("%-7s %u = %s\n", d.name.c_str(),
				static_cast<unsigned>(d.originalType), typeName(d.originalType));

		// -- Typed round-trip: every value lands as a native C++ object.
		sel.execute(tra);
		printf("\nflag  : %s   (C++ bool)\n", *sel.getBool(0) ? "true" : "false");

		const BoostInt128 big = *sel.getBoostInt128(1);
		printf("big   : %s\n", str(big).c_str());
		printf("        == (1 << 127) - 1 computed in Boost? %s\n",
			big == (BoostInt128{1} << 127) - 1 ? "yes" : "NO");

		const BoostDecFloat34 money = *sel.getBoostDecFloat34(2);
		printf("money : %s   == exactly 0.1? %s\n", str(money).c_str(),
			money == BoostDecFloat34{"0.1"} ? "yes" : "NO");

		const TimestampTz born = *sel.getTimestampTz(3);
		printf("born  : %s\n", sel.getString(3)->c_str());
		printf("        zone = \"%s\", UTC instant = %04d-%02u-%02u %02ld:%02ld\n",
			born.zone.c_str(),
			static_cast<int>(born.utcTimestamp.date.year()),
			static_cast<unsigned>(born.utcTimestamp.date.month()),
			static_cast<unsigned>(born.utcTimestamp.date.day()),
			static_cast<long>(born.utcTimestamp.time.hours().count()),
			static_cast<long>(born.utcTimestamp.time.minutes().count()));

		printf("mail  : %s\n", sel.getString(4)->c_str());

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
