/*
 *  parser_errors.cpp (fb-cpp) — the SQL parser seen through typed exceptions.
 *
 *  The fb-cpp twin of ../cpp/parser_errors.cpp: the same six statements fed
 *  to the parser, but here "prepare" is simply constructing a Statement —
 *  fb-cpp prepares in the constructor and always prefetches metadata, so a
 *  successful parse hands back ready-made Descriptor vectors (type, length,
 *  name per parameter/column) instead of raw IMessageMetadata calls, and a
 *  failed parse throws fbcpp::DatabaseException: what() carries the whole
 *  formatted status chain ("Token unknown - line N, column M"), getErrors()
 *  the raw status vector, getErrorCode() the first gds code.
 *  See ../../grammar-and-parser.md.
 *
 *  Read-only against the stock employee database.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_parser_errors [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* typeName(StatementType t)
{
	switch (t)
	{
		case StatementType::SELECT: return "SELECT";
		case StatementType::INSERT: return "INSERT";
		case StatementType::UPDATE: return "UPDATE";
		case StatementType::DDL:    return "DDL";
		default:                    return "other";
	}
}

// Feed one string to the parser; a Statement that constructs successfully
// reports its shape, one that does not throws a DatabaseException.
static void tryPrepare(Attachment& att, Transaction& tra, const char* sql)
{
	printf("---- %s\n", sql);
	try
	{
		Statement stmt{att, tra, sql};
		printf("  parsed OK: type=%s, input params=%zu, output columns=%zu\n",
			typeName(stmt.getType()),
			stmt.getInputDescriptors().size(),
			stmt.getOutputDescriptors().size());
		for (const auto& p : stmt.getInputDescriptors())
			printf("    param: sqltype=%u, length=%u\n",
				static_cast<unsigned>(p.originalType), p.length);
	}
	catch (const DatabaseException& e)
	{
		// The raw status vector: (type, value) pairs up to isc_arg_end.
		unsigned gds = 0, strings = 0, numbers = 0;
		const auto& v = e.getErrors();
		for (size_t i = 0; i + 1 < v.size() && v[i] != isc_arg_end;
			i += (v[i] == isc_arg_cstring ? 3 : 2))
		{
			if (v[i] == isc_arg_gds) ++gds;
			else if (v[i] == isc_arg_string) ++strings;
			else if (v[i] == isc_arg_number) ++numbers;
		}
		printf("  prepare failed: getErrorCode()=%ld; getErrors() holds "
			"%u gds codes, %u strings, %u numbers\n%s\n",
			static_cast<long>(e.getErrorCode()), gds, strings, numbers,
			e.what());
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost/employee");

	try
	{
		Client client{"fbclient"};
		Attachment att{client, database, defaultOptions()};
		Transaction tra{att};

		// 1. Dynamic SQL: the `?` becomes a typed parameter.
		tryPrepare(att, tra,
			"SELECT first_name FROM employee WHERE emp_no = ?");

		// 2. One token, two grammatical roles: FIRST as row-limit clause...
		tryPrepare(att, tra, "SELECT FIRST 1 emp_no FROM employee");
		// ...and FIRST as an ordinary identifier (non-reserved keyword).
		tryPrepare(att, tra,
			"SELECT first FROM (SELECT 1 AS first FROM rdb$database)");

		// 3. Syntax errors with token position.
		tryPrepare(att, tra, "SELEC 1 FROM rdb$database");
		tryPrepare(att, tra,
			"SELECT emp_no\nFROM employee\nWHERE ORDER BY 1");

		// 4. Semantic error — still carries line/column.
		tryPrepare(att, tra, "SELECT frst_name\nFROM employee");

		tra.commit();
		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
