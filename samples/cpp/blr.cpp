/*
 *  blr.cpp — companion to ../../blr-intermediate-language.md
 *
 *  Reads stored BLR *raw* from the catalog of the stock employee database:
 *  the computed column EMPLOYEE.FULL_NAME (RDB$FIELDS.RDB$COMPUTED_BLR) and
 *  the procedure GET_EMP_PROJ (RDB$PROCEDURES.RDB$PROCEDURE_BLR) are fetched
 *  as blobs via IAttachment::openBlob and hex-dumped, then the first bytes
 *  are decoded with the real opcode values from firebird/impl/blr.h — the
 *  same header the engine compiles.  The expression decoder walks the whole
 *  prefix-encoded tree; the procedure decoder stops after the message
 *  declarations (isql's SET BLOB ALL renders the rest symbolically).
 *
 *  Read-only against employee.
 */

#include "fb_sample.h"
#include "firebird/impl/blr.h"

using namespace Firebird;

// Fetch a single-row, single-BLOB-column query and return the blob's bytes.
static std::vector<unsigned char> fetchBlob(fbsample::Db& db, ITransaction* tra,
	const char* sql)
{
	IStatement* stmt = db.att->prepare(&db.status, tra, 0, sql, SQL_DIALECT_V6,
		IStatement::PREPARE_PREFETCH_METADATA);
	IMessageMetadata* meta = stmt->getOutputMetadata(&db.status);
	std::vector<unsigned char> row(meta->getMessageLength(&db.status));
	IResultSet* curs = stmt->openCursor(&db.status, tra, nullptr, nullptr, meta, 0);
	if (curs->fetchNext(&db.status, row.data()) != IStatus::RESULT_OK)
		throw std::runtime_error("no row");
	ISC_QUAD blobId;
	memcpy(&blobId, row.data() + meta->getOffset(&db.status, 0), sizeof blobId);
	curs->close(&db.status);
	meta->release();
	stmt->free(&db.status);

	IBlob* blob = db.att->openBlob(&db.status, tra, &blobId, 0, nullptr);
	std::vector<unsigned char> bytes;
	unsigned char seg[4096];
	for (;;)
	{
		unsigned len = 0;
		const int rc = blob->getSegment(&db.status, sizeof seg, seg, &len);
		if (rc != IStatus::RESULT_OK && rc != IStatus::RESULT_SEGMENT)
			break;
		bytes.insert(bytes.end(), seg, seg + len);
	}
	blob->close(&db.status);
	return bytes;
}

static void hexDump(const std::vector<unsigned char>& b, size_t limit)
{
	for (size_t i = 0; i < b.size() && i < limit; ++i)
		printf("%02x%s", b[i], (i % 16 == 15) ? "\n" : " ");
	printf(b.size() > limit ? "... (%zu bytes total)\n" : "(%zu bytes total)\n",
		b.size());
}

// Decode one *expression* — enough opcodes for a computed column.
static void expr(const unsigned char*& p, const unsigned char* end, int depth)
{
	printf("%*s", depth * 3, "");
	switch (const unsigned char op = *p++; op)
	{
		case blr_concatenate:
			printf("blr_concatenate\n");
			expr(p, end, depth + 1);
			expr(p, end, depth + 1);
			break;
		case blr_field:
		{
			const unsigned ctx = *p++, len = *p++;
			printf("blr_field context %u, '%.*s'\n", ctx, len, p);
			p += len;
			break;
		}
		case blr_literal:
			if (*p == blr_text2)
			{
				++p;
				const unsigned cs = p[0] | (p[1] << 8);
				const unsigned len = p[2] | (p[3] << 8);
				p += 4;
				printf("blr_literal blr_text2 charset %u, len %u, \"%.*s\"\n",
					cs, len, len, p);
				p += len;
			}
			else
				{ printf("blr_literal dtype %u ...\n", *p); p = end; }
			break;
		default:
			printf("opcode %u (decoder stops here)\n", op);
			p = end;
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

		printf("== computed column EMPLOYEE.FULL_NAME — RDB$FIELDS.RDB$COMPUTED_BLR\n");
		auto blr = fetchBlob(db, tra,
			"SELECT f.RDB$COMPUTED_BLR FROM RDB$FIELDS f"
			" JOIN RDB$RELATION_FIELDS rf ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE"
			" WHERE rf.RDB$RELATION_NAME = 'EMPLOYEE'"
			" AND rf.RDB$FIELD_NAME = 'FULL_NAME'");
		hexDump(blr, 64);

		const unsigned char* p = blr.data();
		const unsigned char* end = p + blr.size();
		printf("%s\n", *p++ == blr_version5 ? "blr_version5" : "unexpected version!");
		expr(p, end, 1);
		printf("%s\n", (p < end && *p == blr_eoc) ? "blr_eoc" : "(no blr_eoc?)");

		printf("\n== procedure GET_EMP_PROJ — RDB$PROCEDURES.RDB$PROCEDURE_BLR\n");
		blr = fetchBlob(db, tra,
			"SELECT RDB$PROCEDURE_BLR FROM RDB$PROCEDURES"
			" WHERE RDB$PROCEDURE_NAME = 'GET_EMP_PROJ'");
		hexDump(blr, 32);

		// The opening bytes: version, begin, then the message declarations
		// (the wire-format row layouts this doc and the protocol doc share).
		p = blr.data();
		end = p + blr.size();
		printf("%s, ", *p++ == blr_version5 ? "blr_version5" : "?");
		printf("%s\n", *p++ == blr_begin ? "blr_begin" : "?");
		while (*p == blr_message)
		{
			++p;
			const unsigned msg = *p++;
			const unsigned count = p[0] | (p[1] << 8);
			p += 2;
			printf("blr_message %u, %u fields:", msg, count);
			for (unsigned i = 0; i < count; ++i)
			{
				if (*p == blr_short)
					{ printf(" blr_short(scale %u)", p[1]); p += 2; }
				else if (*p == blr_text2)
				{
					printf(" blr_text2(cs %u, len %u)",
						p[1] | (p[2] << 8), p[3] | (p[4] << 8));
					p += 5;
				}
				else
					{ printf(" dtype %u?", *p); i = count; }
			}
			printf("\n");
		}
		printf("... %zu more bytes — see isql SET BLOB ALL for the full dump\n",
			static_cast<size_t>(end - p));

		tra->commit(&db.status);
	}
	catch (const FbException& error)
	{
		return fbsample::report(error);
	}
	return 0;
}
