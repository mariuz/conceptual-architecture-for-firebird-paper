/*
 *  migration.cpp (fb-cpp) — the type-mapping table, seen through a driver
 *  that already knows every type.
 *
 *  The fb-cpp twin of ../cpp/migration.cpp: the same TYPE_PROBE table with
 *  the types migrations trip over (INT128, NUMERIC(38,8), DECFLOAT(34),
 *  TIMESTAMP WITH TIME ZONE, BOOLEAN, OCTETS/UUID), inspected both ways.
 *  The DESCRIBE face comes from fb-cpp's cached Descriptors — the same
 *  SQL_* codes IMessageMetadata reports, one struct per column instead of
 *  five interface calls.  For the text face the OO-API sample had to coerce
 *  every column to VARCHAR and let the server's CVT rules render strings;
 *  fb-cpp's getString() converts CLIENT-side from the native wire value —
 *  same digits, no server round-trip through text, and the typed getters
 *  (getBoostInt128) prove nothing was truncated on the way.
 *  See ../../migration-and-interoperability.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_migration [database]
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
	case DescriptorOriginalType::TEXT:         return "SQL_TEXT (CHAR)";
	case DescriptorOriginalType::VARYING:      return "SQL_VARYING (VARCHAR)";
	case DescriptorOriginalType::SHORT:        return "SQL_SHORT";
	case DescriptorOriginalType::LONG:         return "SQL_LONG";
	case DescriptorOriginalType::INT64:        return "SQL_INT64";
	case DescriptorOriginalType::INT128:       return "SQL_INT128";
	case DescriptorOriginalType::DOUBLE:       return "SQL_DOUBLE";
	case DescriptorOriginalType::DEC16:        return "SQL_DEC16 (DECFLOAT 16)";
	case DescriptorOriginalType::DEC34:        return "SQL_DEC34 (DECFLOAT 34)";
	case DescriptorOriginalType::TIMESTAMP:    return "SQL_TIMESTAMP";
	case DescriptorOriginalType::TIMESTAMP_TZ: return "SQL_TIMESTAMP_TZ";
	case DescriptorOriginalType::BOOLEAN:      return "SQL_BOOLEAN";
	case DescriptorOriginalType::BLOB:         return "SQL_BLOB";
	default:                                   return "?";
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/migration_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		Transaction tra{att};

		try { Statement{att, tra, "DROP TABLE TYPE_PROBE"}.execute(tra); }
		catch (const DatabaseException&) {}
		Statement{att, tra,
			"CREATE TABLE TYPE_PROBE ("
			"  C_INT128 INT128,"
			"  C_NUM    NUMERIC(38,8),"           // stored in an INT128 too
			"  C_DEC    DECFLOAT(34),"
			"  C_TSTZ   TIMESTAMP WITH TIME ZONE,"
			"  C_BOOL   BOOLEAN,"
			"  C_UUID   CHAR(16) CHARACTER SET OCTETS,"
			"  C_VC     VARCHAR(20))"}.execute(tra);
		tra.commitRetaining();
		Statement{att, tra,
			"INSERT INTO TYPE_PROBE VALUES ("
			"  170141183460469231731687303715884105727,"      // max INT128
			"  123456789012345678901234567890.12345678,"
			"  1.234567890123456789012345678901234E+10,"
			"  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
			"  TRUE, GEN_UUID(), 'naïve ütf8 text')"}.execute(tra);
		tra.commitRetaining();

		// -- 1. what DESCRIBE tells a driver: one cached Descriptor per column
		printf("output Descriptors of SELECT * FROM TYPE_PROBE:\n\n");
		printf("%-8s %6s %-24s %6s %5s\n", "column", "code", "wire type", "length", "scale");
		Statement sel{att, tra, "SELECT * FROM TYPE_PROBE"};
		for (const auto& d : sel.getOutputDescriptors())
			printf("%-8s %6u %-24s %6u %5d\n", d.name.c_str(),
				static_cast<unsigned>(d.originalType), typeName(d.originalType),
				d.length, d.scale);

		// -- 2. the text face: getString() converts client-side ------------
		printf("\nsame row, every column through getString():\n\n");
		sel.execute(tra);
		const char* names[] = { "C_INT128", "C_NUM", "C_DEC", "C_TSTZ", "C_BOOL" };
		for (unsigned i = 0; i < 5; ++i)
			printf("  %-8s = %s\n", names[i], sel.getString(i).value_or("<null>").c_str());
		printf("  %-8s = %s\n", "C_VC", sel.getString(6).value_or("<null>").c_str());
		{
			Statement uuid{att, tra, "SELECT UUID_TO_CHAR(C_UUID) FROM TYPE_PROBE"};
			uuid.execute(tra);
			printf("  %-8s = %s   (rendered via UUID_TO_CHAR)\n", "C_UUID",
				uuid.getString(0)->c_str());
		}

		// -- 3. the typed face: the value a string-copying ETL would risk --
		const BoostInt128 big = *sel.getBoostInt128(0);
		std::ostringstream os;
		os << big;
		printf("\nC_INT128 as a native 128-bit integer: %s\n", os.str().c_str());
		printf("  round-trips through Boost.Multiprecision, no text involved\n");

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
