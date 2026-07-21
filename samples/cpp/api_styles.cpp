/*
 *  api_styles.cpp — the same query through both C-level APIs of fbclient.
 *
 *  Companion to ../../client-apis-and-drivers.md.  The client library
 *  exposes TWO APIs: the legacy ISC API (ibase.h: flat isc_* functions,
 *  status vectors, XSQLDA descriptors) and the modern object-oriented API
 *  (firebird/Interface.h).  This sample runs one identical SELECT through
 *  each, so the contrast — hand-built DPB and descriptor bookkeeping versus
 *  interfaces and exceptions — is visible in a single file.  Both halves
 *  drive the same Y-valve in the same process.
 */

#include <ibase.h>          // the legacy ISC API (InterBase heritage)
#include "fb_sample.h"      // the OO API, wrapped as in every other sample

#include <cstdio>
#include <cstring>
#include <cstdlib>

static const char* SQL =
	"select rdb$get_context('SYSTEM', 'ENGINE_VERSION') from rdb$database";

// ---------------------------------------------------------------- ISC API --

// The classic error idiom: a 20-slot status vector checked after every call,
// rendered with fb_interpret() walking the chain.
static void check(ISC_STATUS* st, const char* where)
{
	if (st[0] == 1 && st[1])
	{
		char msg[512];
		const ISC_STATUS* walk = st;
		fprintf(stderr, "ISC error in %s:\n", where);
		while (fb_interpret(msg, sizeof(msg), &walk))
			fprintf(stderr, "    %s\n", msg);
		exit(1);
	}
}

static void iscStyle(const char* database, const char* user, const char* password)
{
	ISC_STATUS_ARRAY st;
	isc_db_handle  db   = 0;
	isc_tr_handle  tr   = 0;
	isc_stmt_handle stmt = 0;

	// 1. Build the DPB by hand: tag, length byte, payload.
	char dpb[128];
	char* p = dpb;
	*p++ = isc_dpb_version1;
	*p++ = isc_dpb_user_name;
	*p++ = (char) strlen(user);
	memcpy(p, user, strlen(user));       p += strlen(user);
	*p++ = isc_dpb_password;
	*p++ = (char) strlen(password);
	memcpy(p, password, strlen(password)); p += strlen(password);

	isc_attach_database(st, 0, database, &db, (short) (p - dpb), dpb);
	check(st, "isc_attach_database");

	isc_start_transaction(st, &tr, 1, &db, 0, nullptr);
	check(st, "isc_start_transaction");

	// 2. The DSQL statement lifecycle: allocate, prepare, describe, execute, fetch.
	isc_dsql_allocate_statement(st, &db, &stmt);
	check(st, "isc_dsql_allocate_statement");

	XSQLDA* out = (XSQLDA*) malloc(XSQLDA_LENGTH(1));
	out->version = SQLDA_VERSION1;
	out->sqln = 1;
	isc_dsql_prepare(st, &tr, &stmt, 0, SQL, SQL_DIALECT_V6, out);
	check(st, "isc_dsql_prepare");

	// 3. Bind the one output column: caller supplies data + null-indicator
	//    storage and must honour the declared type (VARCHAR: 2-byte length
	//    prefix, then the bytes).
	struct { short len; char data[512]; } var;
	short nullInd = 0;
	XSQLVAR* v = &out->sqlvar[0];
	v->sqltype = SQL_VARYING + 1;        // +1: nullable, sqlind is used
	v->sqldata = (char*) &var;
	v->sqlind  = &nullInd;

	isc_dsql_execute(st, &tr, &stmt, SQL_DIALECT_V6, nullptr);
	check(st, "isc_dsql_execute");

	while (isc_dsql_fetch(st, &stmt, SQL_DIALECT_V6, out) == 0)
		printf("[ISC API] engine version = %.*s\n", var.len, var.data);
	check(st, "isc_dsql_fetch");

	isc_dsql_free_statement(st, &stmt, DSQL_drop);
	isc_commit_transaction(st, &tr);
	isc_detach_database(st, &db);
	check(st, "isc_detach_database");
	free(out);
}

// ----------------------------------------------------------------- OO API --

static void ooStyle(const char* database)
{
	fbsample::Db db;                     // IMaster -> IProvider, RAII cleanup
	db.attach(database);                 // DPB via IXpbBuilder
	Firebird::ITransaction* tra = db.start();
	printf("[OO API ] engine version = %s\n", db.queryValue(tra, SQL).c_str());
	tra->commit(&db.status);
}

int main(int argc, char** argv)
{
	const char* database = fbsample::argOrDefault(argc, argv, 1,
		"inet://localhost/employee");

	iscStyle(database, fbsample::Db::defaultUser(), fbsample::Db::defaultPassword());

	try
	{
		ooStyle(database);
	}
	catch (const Firebird::FbException& e)
	{
		return fbsample::report(e);
	}

	printf("same engine, same Y-valve, two API styles. done.\n");
	return 0;
}
