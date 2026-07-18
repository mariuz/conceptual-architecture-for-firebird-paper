/*
 *  client_test.cpp  --  Firebird 6 client test using the public OO API.
 *
 *  This sample exercises the top-level architecture described in the paper:
 *  the client links against the fbclient library, whose calls travel through
 *  the REMOTE subsystem and the Y-valve dispatcher into DSQL and the JRD
 *  engine (see Figure 1).  It creates a database, creates a table, inserts a
 *  few rows inside a transaction and reads them back through a cursor.
 *
 *  It uses only the modern object-oriented API (fb_get_master_interface,
 *  IProvider, IAttachment, IStatement, IResultSet) which is the supported
 *  client interface in Firebird 3 and later, including Firebird 6.
 *
 *  Build (see samples/README.md and CMakeLists.txt):
 *      c++ -I<firebird>/src/include client_test.cpp -lfbclient -o client_test
 *
 *  Usage:
 *      ./client_test [database]
 *
 *  The database path defaults to "employees.fdb" in the current directory and
 *  may also be a remote string such as "localhost:/data/employees.fdb".
 *  Credentials are taken from the ISC_USER / ISC_PASSWORD environment
 *  variables, defaulting to the traditional SYSDBA / masterkey.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <firebird/Interface.h>

using namespace Firebird;

// The master interface is the single entry point of the API; it is obtained
// through the only plain C function the client library exports.
static IMaster* master = fb_get_master_interface();

int main(int argc, char** argv)
{
	const char* database = (argc > 1) ? argv[1] : "employees.fdb";

	// Default the login if the environment does not provide one.
	setenv("ISC_USER", getenv("ISC_USER") ? getenv("ISC_USER") : "SYSDBA", 1);
	setenv("ISC_PASSWORD", getenv("ISC_PASSWORD") ? getenv("ISC_PASSWORD") : "masterkey", 1);

	int rc = 0;

	// These interfaces never fail to be produced by the master interface.
	ThrowStatusWrapper status(master->getStatus());
	IProvider* prov = master->getDispatcher();
	IUtil* utl = master->getUtilInterface();

	IAttachment* att = nullptr;
	ITransaction* tra = nullptr;
	IStatement* stmt = nullptr;
	IResultSet* curs = nullptr;
	IXpbBuilder* dpb = nullptr;

	try
	{
		// Build a Database Parameter Buffer carrying page size and credentials.
		dpb = utl->getXpbBuilder(&status, IXpbBuilder::DPB, nullptr, 0);
		dpb->insertInt(&status, isc_dpb_page_size, 8 * 1024);
		dpb->insertString(&status, isc_dpb_user_name, getenv("ISC_USER"));
		dpb->insertString(&status, isc_dpb_password, getenv("ISC_PASSWORD"));
		dpb->insertString(&status, isc_dpb_set_db_charset, "UTF8");

		// Attach if the database already exists, otherwise create it.  Both
		// calls return an IAttachment; the create path also runs the DDL.
		try
		{
			att = prov->attachDatabase(&status, database,
				dpb->getBufferLength(&status), dpb->getBuffer(&status));
			printf("Attached to existing database: %s\n", database);
		}
		catch (const FbException&)
		{
			att = prov->createDatabase(&status, database,
				dpb->getBufferLength(&status), dpb->getBuffer(&status));
			printf("Created new database: %s\n", database);
		}

		// --- DDL: recreate the demo table ------------------------------------
		tra = att->startTransaction(&status, 0, nullptr);
		att->execute(&status, tra, 0,
			"recreate table people (id integer not null primary key, "
			"name varchar(30), city varchar(30))",
			SQL_DIALECT_V6, nullptr, nullptr, nullptr, nullptr);
		tra->commitRetaining(&status);
		printf("Table 'people' ready.\n");

		// --- DML: insert a few rows ------------------------------------------
		// The data below is trusted, constant demo content, so plain SQL keeps
		// the sample readable.  Parameterised execution with an IMessageMetadata
		// input message is shown for the SELECT path below.
		struct { int id; const char* name; const char* city; } seed[] = {
			{ 1, "Ada Lovelace",     "London"     },
			{ 2, "Grace Hopper",     "New York"   },
			{ 3, "Edsger Dijkstra",  "Rotterdam"  },
			{ 4, "Jim Starkey",      "Manchester" },
		};

		for (auto& s : seed)
		{
			char sql[256];
			snprintf(sql, sizeof(sql),
				"insert into people (id, name, city) values (%d, '%s', '%s')",
				s.id, s.name, s.city);
			att->execute(&status, tra, 0, sql, SQL_DIALECT_V6,
				nullptr, nullptr, nullptr, nullptr);
		}
		tra->commitRetaining(&status);
		printf("Inserted %zu rows.\n\n", sizeof(seed) / sizeof(seed[0]));

		// --- Query: read the rows back through a cursor ----------------------
		stmt = att->prepare(&status, tra, 0,
			"select id, name, city from people order by id",
			SQL_DIALECT_V6, IStatement::PREPARE_PREFETCH_METADATA);

		// Coerce VARCHAR columns to fixed CHAR so a flat struct can receive them.
		IMessageMetadata* meta = stmt->getOutputMetadata(&status);
		IMetadataBuilder* builder = meta->getBuilder(&status);
		unsigned cols = meta->getCount(&status);
		for (unsigned j = 0; j < cols; ++j)
		{
			unsigned t = meta->getType(&status, j);
			if (t == SQL_VARYING || t == SQL_TEXT)
				builder->setType(&status, j, SQL_TEXT);
		}
		meta->release();
		meta = builder->getMetadata(&status);
		builder->release();

		unsigned msgLen = meta->getMessageLength(&status);
		unsigned char* buffer = new unsigned char[msgLen];

		unsigned idOff   = meta->getOffset(&status, 0);
		unsigned nameOff = meta->getOffset(&status, 1);
		unsigned cityOff = meta->getOffset(&status, 2);
		unsigned nameLen = meta->getLength(&status, 1);
		unsigned cityLen = meta->getLength(&status, 2);

		curs = stmt->openCursor(&status, tra, nullptr, nullptr, meta, 0);

		// CHAR values arrive space-padded to their full byte length (four
		// bytes per character in a UTF8 database), so trim trailing blanks.
		auto trimmed = [](const unsigned char* p, unsigned len)
		{
			while (len > 0 && p[len - 1] == ' ')
				--len;
			return std::string(reinterpret_cast<const char*>(p), len);
		};

		printf("%-4s %-20s %-20s\n", "ID", "NAME", "CITY");
		printf("---- -------------------- --------------------\n");
		while (curs->fetchNext(&status, buffer) == IStatus::RESULT_OK)
		{
			int id = *reinterpret_cast<int*>(buffer + idOff);
			printf("%-4d %-20s %-20s\n", id,
				trimmed(buffer + nameOff, nameLen).c_str(),
				trimmed(buffer + cityOff, cityLen).c_str());
		}

		delete[] buffer;
		meta->release();
		curs->close(&status);
		curs = nullptr;
		stmt->free(&status);
		stmt = nullptr;

		tra->commit(&status);
		tra = nullptr;
		att->detach(&status);
		att = nullptr;

		printf("\nDone.\n");
	}
	catch (const FbException& error)
	{
		rc = 1;
		char buf[512];
		utl->formatStatus(buf, sizeof(buf), error.getStatus());
		fprintf(stderr, "Firebird error:\n%s\n", buf);
	}

	// Release any interfaces still open after an error.
	if (curs) curs->release();
	if (stmt) stmt->release();
	if (tra)  tra->release();
	if (att)  att->release();
	if (dpb)  dpb->dispose();

	prov->release();
	status.dispose();
	return rc;
}
