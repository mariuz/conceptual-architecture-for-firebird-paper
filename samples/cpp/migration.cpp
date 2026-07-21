/*
 *  migration.cpp — companion sample for ../../migration-and-interoperability.md
 *
 *  The type-mapping table of the document, made concrete: a probe table with
 *  the Firebird types that migrations trip over (INT128, DECFLOAT(34),
 *  NUMERIC(38,8), TIMESTAMP WITH TIME ZONE, BOOLEAN, OCTETS/UUID), inspected
 *  the two ways a migration tool sees them —
 *
 *    1. the DESCRIBED metadata: the raw SQL_* wire type codes a driver must
 *       recognize (or fail on) — IMessageMetadata::getType() et al.;
 *    2. the TEXT face: every value fetched with the output coerced to
 *       VARCHAR, i.e. the engine's own string rendering that a generic
 *       "select and copy strings" ETL receives.
 */

#include "fb_sample.h"

using namespace Firebird;

static const char* DB = "inet://localhost//tmp/fbhandson/migration.fdb";

static const char* typeName(unsigned t)
{
	switch (t & ~1u)   // strip the nullable bit
	{
	case SQL_TEXT:         return "SQL_TEXT (CHAR)";
	case SQL_VARYING:      return "SQL_VARYING (VARCHAR)";
	case SQL_SHORT:        return "SQL_SHORT";
	case SQL_LONG:         return "SQL_LONG";
	case SQL_INT64:        return "SQL_INT64";
	case SQL_INT128:       return "SQL_INT128";
	case SQL_DOUBLE:       return "SQL_DOUBLE";
	case SQL_DEC16:        return "SQL_DEC16 (DECFLOAT 16)";
	case SQL_DEC34:        return "SQL_DEC34 (DECFLOAT 34)";
	case SQL_TIMESTAMP:    return "SQL_TIMESTAMP";
	case SQL_TIMESTAMP_TZ: return "SQL_TIMESTAMP_TZ";
	case SQL_BOOLEAN:      return "SQL_BOOLEAN";
	case SQL_BLOB:         return "SQL_BLOB";
	default:               return "?";
	}
}

int main(int argc, char** argv)
{
	try
	{
		fbsample::Db db;
		db.attachOrCreate(fbsample::argOrDefault(argc, argv, 1, DB));
		ITransaction* tra = db.start();

		try { db.exec(tra, "DROP TABLE TYPE_PROBE"); } catch (const FbException&) {}
		db.exec(tra,
			"CREATE TABLE TYPE_PROBE ("
			"  C_INT128 INT128,"
			"  C_NUM    NUMERIC(38,8),"           // stored in an INT128 too
			"  C_DEC    DECFLOAT(34),"
			"  C_TSTZ   TIMESTAMP WITH TIME ZONE,"
			"  C_BOOL   BOOLEAN,"
			"  C_UUID   CHAR(16) CHARACTER SET OCTETS,"
			"  C_VC     VARCHAR(20))");
		tra->commitRetaining(&db.status);
		db.exec(tra,
			"INSERT INTO TYPE_PROBE VALUES ("
			"  170141183460469231731687303715884105727,"      // max INT128
			"  123456789012345678901234567890.12345678,"
			"  1.234567890123456789012345678901234E+10,"
			"  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
			"  TRUE, GEN_UUID(), 'naïve ütf8 text')");
		tra->commitRetaining(&db.status);

		// -- 1. what DESCRIBE tells a driver: the wire type codes -----------
		printf("described output metadata of SELECT * FROM TYPE_PROBE:\n\n");
		printf("%-8s %6s %-24s %6s %5s\n", "column", "code", "wire type", "length", "scale");
		IStatement* stmt = db.att->prepare(&db.status, tra, 0,
			"SELECT * FROM TYPE_PROBE", SQL_DIALECT_V6,
			IStatement::PREPARE_PREFETCH_METADATA);
		IMessageMetadata* meta = stmt->getOutputMetadata(&db.status);
		for (unsigned i = 0; i < meta->getCount(&db.status); ++i)
		{
			const unsigned t = meta->getType(&db.status, i);
			printf("%-8s %6u %-24s %6u %5d\n",
				meta->getField(&db.status, i), t, typeName(t),
				meta->getLength(&db.status, i), meta->getScale(&db.status, i));
		}
		meta->release();
		stmt->free(&db.status);

		// -- 2. the text face: engine-rendered strings, one per column ------
		printf("\nsame row fetched with every column coerced to VARCHAR:\n\n");
		fbsample::Db::Table t = db.query(tra,
			"SELECT C_INT128, C_NUM, C_DEC, C_TSTZ, C_BOOL, C_VC FROM TYPE_PROBE");
		for (size_t c = 0; c < t.names.size(); ++c)
			printf("  %-8s = %s\n", t.names[c].c_str(), t.rows[0][c].c_str());
		printf("  %-8s = %s   (rendered via UUID_TO_CHAR)\n", "C_UUID",
			db.queryValue(tra, "SELECT UUID_TO_CHAR(C_UUID) FROM TYPE_PROBE").c_str());

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
}
