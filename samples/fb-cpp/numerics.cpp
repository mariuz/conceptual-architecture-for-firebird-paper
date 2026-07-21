/*
 *  numerics.cpp (fb-cpp) — exactness, scaled integers, and traps, typed.
 *
 *  The fb-cpp twin of ../cpp/numerics.cpp: the same four experiments.  The
 *  instructive diffs: the DOUBLE residue arrives as a C++ double and the
 *  DECFLOAT result as a real 34-digit BoostDecFloat34 (compared against
 *  zero in C++, not by string); the document's "scaled integer" claim needs
 *  no raw buffer walk — getScaledInt64() returns the {value, scale} pair
 *  straight from the wire, and the Descriptor shows INT64/scale -4; INT128
 *  overflow and the DECFLOAT trap surface as typed DatabaseExceptions with
 *  getErrorCode().  See ../../numeric-and-precision-arithmetic.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_numerics [database]
 */

#include "fbcpp_sample.h"
#include <cinttypes>
#include <cstdio>
#include <sstream>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

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
		"inet://localhost//tmp/fbhandson/numerics_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		Transaction tra{att};

		// -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3, natively typed.
		{
			Statement s{att, tra,
				"SELECT (CAST(0.1 AS DOUBLE PRECISION) + 0.2) - 0.3,"
				"       (CAST(0.1 AS DECFLOAT(34)) + 0.2) - 0.3 FROM RDB$DATABASE"};
			s.execute(tra);
			const double binary = *s.getDouble(0);
			const BoostDecFloat34 decimal = *s.getBoostDecFloat34(1);
			printf("(0.1+0.2)-0.3 in DOUBLE PRECISION : %.17g   (a C++ double)\n", binary);
			printf("(0.1+0.2)-0.3 in DECFLOAT(34)     : %s   == 0 in C++? %s\n\n",
				str(decimal).c_str(),
				decimal == BoostDecFloat34{0} ? "yes" : "NO");
		}

		// -- 2. NUMERIC(18,4) on the wire: scaled integer + scale, typed.
		{
			Statement s{att, tra,
				"SELECT CAST(12345.6789 AS NUMERIC(18,4)) FROM RDB$DATABASE"};
			const auto& d = s.getOutputDescriptors()[0];
			printf("NUMERIC(18,4) Descriptor: type=%u (%s), length=%u, scale=%d\n",
				static_cast<unsigned>(d.originalType),
				d.originalType == DescriptorOriginalType::INT64 ? "SQL_INT64" : "?",
				d.length, d.scale);
			s.execute(tra);
			const ScaledInt64 raw = *s.getScaledInt64(0);
			printf("getScaledInt64()        : value=%" PRId64 "  scale=%d\n",
				raw.value, raw.scale);
			printf("value = raw * 10^scale  : %" PRId64 " * 10^%d = %.4f\n\n",
				raw.value, raw.scale, double(raw.value) * 1e-4);
		}

		// -- 3. INT128: the full range, and one step past it.
		{
			Statement s{att, tra,
				"SELECT CAST(170141183460469231731687303715884105727 AS INT128)"
				" FROM RDB$DATABASE"};
			s.execute(tra);
			printf("INT128 max  : %s   (a Boost.Multiprecision int128)\n",
				str(*s.getBoostInt128(0)).c_str());
		}
		try
		{
			Statement s{att, tra,
				"SELECT CAST(170141183460469231731687303715884105727 AS INT128) + 1"
				" FROM RDB$DATABASE"};
			s.execute(tra);
			printf("BUG: overflow not detected\n");
		}
		catch (const DatabaseException& e)
		{
			printf("INT128 max+1: gds %ld — %s\n\n",
				static_cast<long>(e.getErrorCode()), firstLine(e.what()).c_str());
		}

		// -- 4. DECFLOAT division by zero: trapped by default, Infinity after.
		try
		{
			Statement s{att, tra,
				"SELECT CAST(1 AS DECFLOAT(16)) / 0 FROM RDB$DATABASE"};
			s.execute(tra);
			printf("BUG: default trap did not fire\n");
		}
		catch (const DatabaseException& e)
		{
			printf("1/0 with default traps : gds %ld — %s\n",
				static_cast<long>(e.getErrorCode()), firstLine(e.what()).c_str());
		}
		Statement{att, tra, "SET DECFLOAT TRAPS TO"}.execute(tra);	// clear all traps
		{
			Statement s{att, tra,
				"SELECT CAST(1 AS DECFLOAT(16)) / 0 FROM RDB$DATABASE"};
			s.execute(tra);
			printf("1/0 with traps cleared : %s\n", s.getString(0)->c_str());
		}

		tra.commit();
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
