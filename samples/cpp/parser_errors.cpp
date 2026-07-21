/*
 *  parser_errors.cpp — companion to ../../grammar-and-parser.md
 *
 *  Drives Firebird's SQL parser from the client through IAttachment::prepare:
 *    1. a dynamic-SQL statement with a `?` placeholder — the parser turns it
 *       into a typed input parameter (dsql/parse.y builds the node tree,
 *       semantic pass resolves the type from the column);
 *    2. two statements only a backtracking grammar accepts comfortably —
 *       FIRST as the FIRST-clause keyword and FIRST as a plain column name;
 *    3. syntax errors — the lexer/parser reports the offending token with its
 *       exact line and column ("Token unknown - line N, column M");
 *    4. a semantic error — position tracking survives past the parse into the
 *       DSQL pass ("Column unknown ... At line N, column M").
 *
 *  Read-only against the stock employee database.
 */

#include "fb_sample.h"

using namespace Firebird;
using fbsample::master;

static const char* typeName(unsigned t)
{
	switch (t)
	{
		case isc_info_sql_stmt_select: return "SELECT";
		case isc_info_sql_stmt_insert: return "INSERT";
		case isc_info_sql_stmt_update: return "UPDATE";
		case isc_info_sql_stmt_ddl:    return "DDL";
		default:                       return "other";
	}
}

// Feed one string to the parser; report either the parsed statement's shape
// or the status vector the parser sent back.
static void tryPrepare(fbsample::Db& db, ITransaction* tra, const char* sql)
{
	printf("---- %s\n", sql);
	try
	{
		IStatement* stmt = db.att->prepare(&db.status, tra, 0, sql,
			SQL_DIALECT_V6, IStatement::PREPARE_PREFETCH_METADATA);

		IMessageMetadata* in  = stmt->getInputMetadata(&db.status);
		IMessageMetadata* out = stmt->getOutputMetadata(&db.status);
		printf("  parsed OK: type=%s, input params=%u, output columns=%u\n",
			typeName(stmt->getType(&db.status)),
			in->getCount(&db.status), out->getCount(&db.status));
		for (unsigned i = 0; i < in->getCount(&db.status); ++i)
			printf("    param %u: sqltype=%u, length=%u\n", i,
				in->getType(&db.status, i), in->getLength(&db.status, i));
		in->release();
		out->release();
		stmt->free(&db.status);
	}
	catch (const FbException& e)
	{
		char buf[1024];
		master->getUtilInterface()->formatStatus(buf, sizeof buf, e.getStatus());
		printf("  prepare failed:\n%s\n", buf);
	}
}

int main(int argc, char** argv)
{
	const char* database = fbsample::argOrDefault(argc, argv, 1,
		"inet://localhost/employee");

	try
	{
		fbsample::Db db;
		db.attach(database, fbsample::Db::defaultUser(),
			fbsample::Db::defaultPassword(), "NONE");
		ITransaction* tra = db.start();

		// 1. Dynamic SQL: the `?` becomes a typed parameter.
		tryPrepare(db, tra,
			"SELECT first_name FROM employee WHERE emp_no = ?");

		// 2. One token, two grammatical roles: FIRST as row-limit clause...
		tryPrepare(db, tra, "SELECT FIRST 1 emp_no FROM employee");
		// ...and FIRST as an ordinary identifier (non-reserved keyword).
		tryPrepare(db, tra,
			"SELECT first FROM (SELECT 1 AS first FROM rdb$database)");

		// 3. Syntax errors with token position.
		tryPrepare(db, tra, "SELEC 1 FROM rdb$database");
		tryPrepare(db, tra,
			"SELECT emp_no\nFROM employee\nWHERE ORDER BY 1");

		// 4. Semantic error — still carries line/column.
		tryPrepare(db, tra, "SELECT frst_name\nFROM employee");

		tra->commit(&db.status);
		printf("done.\n");
	}
	catch (const FbException& error)
	{
		return fbsample::report(error);
	}
	return 0;
}
