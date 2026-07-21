/*
 *  types.cpp — companion sample for sql-dialect-and-types.md.
 *
 *  Round-trips the standout Firebird types from client code: BOOLEAN,
 *  INT128, DECFLOAT(34), TIMESTAMP WITH TIME ZONE and a CHECK-constrained
 *  domain.  Two things are on display:
 *
 *   1. What the wire actually carries — the prepared statement's output
 *      metadata reports the SQL_* type code of every column, straight from
 *      the engine's descriptor (struct dsc, src/common/dsc.h).
 *   2. What the engine's own text conversion makes of each value —
 *      fb_sample.h coerces every output column to VARCHAR, so the strings
 *      below are produced by the server's CVT rules, not by client code.
 *
 *  Build/run: see ../../sql-dialect-and-types.md (Hands-on section).
 */

#include "fb_sample.h"

using namespace fbsample;

// The SQL_* codes returned by IMessageMetadata::getType (include/firebird/impl/sqlda_pub.h).
static const char* typeName(unsigned t)
{
	switch (t)
	{
	case SQL_VARYING: return "SQL_VARYING (VARCHAR)";
	case SQL_TEXT: return "SQL_TEXT (CHAR)";
	case SQL_BOOLEAN: return "SQL_BOOLEAN";
	case SQL_INT128: return "SQL_INT128";
	case SQL_DEC34: return "SQL_DEC34 (DECFLOAT(34))";
	case SQL_TIMESTAMP_TZ: return "SQL_TIMESTAMP_TZ";
	case SQL_INT64: return "SQL_INT64 (BIGINT/NUMERIC)";
	default: return "(other)";
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/types.fdb");
	try
	{
		Db db;
		db.attachOrCreate(database);

		// -- Idempotent cleanup.  Each drop commits on its own: DDL is partly
		//    deferred to commit time (DFW), so DROP DOMAIN's dependency check
		//    must run after the table drop is actually committed.
		for (const char* sql : { "DROP TABLE showcase", "DROP DOMAIN d_email" })
		{
			ITransaction* t = db.start();
			try
			{
				db.exec(t, sql);
				t->commit(&db.status);
			}
			catch (const FbException&)
			{
				CheckStatusWrapper quiet(master->getStatus());
				t->rollback(&quiet);
				quiet.dispose();
			}
		}

		// -- DDL: a domain plus one table using the FB3/FB4 headline types.
		ITransaction* ddl = db.start();
		db.exec(ddl,
			"CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')");
		db.exec(ddl,
			"CREATE TABLE showcase ("
			"  flag  BOOLEAN,"
			"  big   INT128,"
			"  money DECFLOAT(34),"
			"  born  TIMESTAMP WITH TIME ZONE,"
			"  mail  d_email)");
		ddl->commit(&db.status);

		// -- One row exercising each type's edge.
		ITransaction* tra = db.start();
		db.exec(tra,
			"INSERT INTO showcase VALUES ("
			"  TRUE,"
			"  170141183460469231731687303715884105727,"	// INT128 max
			"  0.1,"										// exact in DECFLOAT
			"  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"	// named zone
			"  'user@example.com')");

		// The domain's CHECK travels with the type: a mail without '@' dies.
		try
		{
			db.exec(tra, "INSERT INTO showcase (mail) VALUES ('not-an-address')");
			printf("BUG: domain CHECK did not fire\n");
		}
		catch (const FbException& e)
		{
			char buf[256];
			db.util->formatStatus(buf, sizeof buf, e.getStatus());
			printf("domain CHECK rejected 'not-an-address':\n    %.120s\n\n", buf);
		}

		// -- What the wire carries: type codes from the untouched metadata.
		IStatement* stmt = db.att->prepare(&db.status, tra, 0,
			"SELECT * FROM showcase", SQL_DIALECT_V6,
			IStatement::PREPARE_PREFETCH_METADATA);
		IMessageMetadata* meta = stmt->getOutputMetadata(&db.status);
		printf("column  wire type (IMessageMetadata::getType)\n");
		printf("------  --------------------------------------\n");
		for (unsigned i = 0; i < meta->getCount(&db.status); ++i)
			printf("%-7s %u = %s\n", meta->getField(&db.status, i),
				meta->getType(&db.status, i) & ~1u,
				typeName(meta->getType(&db.status, i) & ~1u));
		meta->release();
		stmt->free(&db.status);

		// -- Round-trip: the engine's own text conversion of every value.
		printf("\n");
		Db::print(db.query(tra, "SELECT * FROM showcase"));

		tra->commit(&db.status);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
