/*
 *  blobs.cpp — companion to ../../blob-handling.md
 *
 *  The segmented BLOB API end to end: create a text blob with three explicit
 *  putSegment() calls, store it through a parameterized INSERT (the record
 *  holds only the 8-byte blob id), read it back segment by segment with
 *  getSegment() — the boundaries survive — and ask the blob itself for its
 *  statistics via getInfo() (segment count, longest segment, total length,
 *  segmented vs stream type).  Then the catalog view of subtype/charset for
 *  a TEXT vs BINARY column, and BLOB_APPEND assembling a blob in SQL.
 */

#include "fb_sample.h"
#include <firebird/impl/inf_pub.h>

using namespace fbsample;

// Decode the {item, 2-byte length, value} clumplets a getInfo() call returns.
static long infoValue(const unsigned char* buf, unsigned len, unsigned char item)
{
	for (const unsigned char* p = buf; p < buf + len && *p != isc_info_end;)
	{
		const unsigned char it = *p++;
		const unsigned short clen = (unsigned short) (p[0] | (p[1] << 8));
		p += 2;
		if (it == item)
		{
			long v = 0;
			for (unsigned i = 0; i < clen && i < 4; ++i)
				v |= long(p[i]) << (8 * i);
			return v;
		}
		p += clen;
	}
	return -1;
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost//tmp/fbhandson/blobs.fdb");

	try
	{
		Db db;
		db.attachOrCreate(database);
		ITransaction* tra = db.start();
		db.exec(tra, "recreate table docs ("
			" id integer primary key,"
			" note blob sub_type text character set utf8,"
			" data blob sub_type binary)");
		tra->commit(&db.status);

		tra = db.start();

		// -- 1. create a blob with three explicit segments ------------------
		ISC_QUAD noteId;
		IBlob* blob = db.att->createBlob(&db.status, tra, &noteId, 0, nullptr);
		const char* segments[] = { "first segment", "second, longer segment", "third" };
		for (const char* seg : segments)
			blob->putSegment(&db.status, (unsigned) strlen(seg), seg);
		blob->close(&db.status);
		printf("wrote 3 segments into a new text blob\n");

		// Store it: the row itself receives only the 8-byte blob id.
		IStatement* stmt = db.att->prepare(&db.status, tra, 0,
			"insert into docs (id, note) values (?, ?)", SQL_DIALECT_V6, 0);
		IMessageMetadata* in = stmt->getInputMetadata(&db.status);
		std::vector<unsigned char> msg(in->getMessageLength(&db.status), 0);
		const int id = 1;
		memcpy(msg.data() + in->getOffset(&db.status, 0), &id, sizeof id);
		memcpy(msg.data() + in->getOffset(&db.status, 1), &noteId, sizeof noteId);
		stmt->execute(&db.status, tra, in, msg.data(), nullptr, nullptr);
		in->release();
		stmt->free(&db.status);

		// -- 2. read it back: getSegment preserves the boundaries -----------
		stmt = db.att->prepare(&db.status, tra, 0,
			"select note from docs where id = 1", SQL_DIALECT_V6, 0);
		IMessageMetadata* out = stmt->getOutputMetadata(&db.status);
		std::vector<unsigned char> row(out->getMessageLength(&db.status));
		IResultSet* rs = stmt->openCursor(&db.status, tra, nullptr, nullptr, nullptr, 0);
		rs->fetchNext(&db.status, row.data());
		ISC_QUAD readId;
		memcpy(&readId, row.data() + out->getOffset(&db.status, 0), sizeof readId);
		rs->close(&db.status);
		out->release();
		stmt->free(&db.status);

		blob = db.att->openBlob(&db.status, tra, &readId, 0, nullptr);
		char seg[64];
		unsigned segLen, n = 0;
		while (blob->getSegment(&db.status, sizeof seg - 1, seg, &segLen) == IStatus::RESULT_OK)
		{
			seg[segLen] = 0;
			printf("  getSegment #%u: %2u bytes  \"%s\"\n", ++n, segLen, seg);
		}

		// -- 3. the blob describes itself: getInfo --------------------------
		const unsigned char items[] = { isc_info_blob_num_segments,
			isc_info_blob_max_segment, isc_info_blob_total_length, isc_info_blob_type };
		unsigned char info[64];
		blob->getInfo(&db.status, sizeof items, items, sizeof info, info);
		printf("blob info: %ld segments, longest %ld, total %ld bytes, type %ld (0=segmented)\n",
			infoValue(info, sizeof info, isc_info_blob_num_segments),
			infoValue(info, sizeof info, isc_info_blob_max_segment),
			infoValue(info, sizeof info, isc_info_blob_total_length),
			infoValue(info, sizeof info, isc_info_blob_type));
		blob->close(&db.status);

		// -- 4. subtype text vs binary, from the catalog --------------------
		printf("\n-- column subtypes (RDB$FIELDS) --\n");
		Db::Table sub = db.query(tra,
			"select trim(rf.rdb$field_name), f.rdb$field_sub_type, trim(cs.rdb$character_set_name) "
			"from rdb$relation_fields rf "
			"join rdb$fields f on rf.rdb$field_source = f.rdb$field_name "
			"left join rdb$character_sets cs "
			"  on f.rdb$character_set_id = cs.rdb$character_set_id "
			"where rf.rdb$relation_name = 'DOCS' and f.rdb$field_type = 261 "
			"order by 1");
		sub.names = { "FIELD", "SUBTYPE", "CHARSET" };
		Db::print(sub);

		// -- 5. BLOB_APPEND: build a blob in SQL without recopying ----------
		db.exec(tra, "insert into docs (id, note) values (2, "
			"blob_append(cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))");
		printf("\n-- BLOB_APPEND result --\n");
		Db::Table ba = db.query(tra,
			"select id, octet_length(note), char_length(note), cast(note as varchar(50)) "
			"from docs where id = 2");
		ba.names = { "ID", "OCTETS", "CHARS", "CONTENT" };
		Db::print(ba);

		tra->commit(&db.status);
		db.att->detach(&db.status);
		db.att = nullptr;
		printf("done.\n");
		return 0;
	}
	catch (const FbException& error)
	{
		return report(error);
	}
}
