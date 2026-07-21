/*
 *  blobs.cpp (fb-cpp) — companion to ../../blob-handling.md
 *
 *  The fb-cpp twin of ../cpp/blobs.cpp, and the one sample where the
 *  wrapper's own abstraction is the subject: fbcpp::Blob is a RAII object
 *  with writeSegment/readSegment (the segmented model, boundaries kept),
 *  write/read (chunked convenience), getLength, and seek — and its
 *  BlobOptions builder turns the raw BPB into named calls like
 *  setType(BlobType::STREAM).  The sample replays the OO-API scenario
 *  (three explicit segments, stored via a parameterized INSERT that
 *  carries only the 8-byte BlobId, read back segment by segment), adds a
 *  STREAM twin of the same bytes to show seek() working where a
 *  segmented blob would refuse it, and drops to getHandle()->getInfo()
 *  for the one thing fb-cpp does not surface: the blh_count /
 *  blh_max_segment bookkeeping.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_blobs [database]
 */

#include "fbcpp_sample.h"
#include <firebird/impl/inf_pub.h>
#include <cstring>
#include <string>
#include <vector>

using namespace fbcpp;
using namespace fbcpp_sample;

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
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/blobs_fbcpp.fdb");

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		{
			Transaction t{att};
			att.execute(t, "recreate table docs ("
				" id integer primary key,"
				" note blob sub_type text character set utf8,"
				" data blob sub_type binary)");
			t.commit();
		}

		Transaction tra{att};
		const char* segments[] = { "first segment", "second, longer segment", "third" };

		// -- 1. create a blob with three explicit segments ------------------
		Blob note{att, tra};					// default: segmented
		for (const char* seg : segments)
			note.writeSegment(std::span<const char>{seg, strlen(seg)});
		note.close();
		printf("wrote 3 segments into a new text blob\n");

		// Store it: the row itself receives only the 8-byte blob id.
		Statement ins{att, tra, "insert into docs (id, note) values (?, ?)"};
		ins.set(0, 1);
		ins.setBlobId(1, note.getId());
		ins.execute(tra);

		// -- 2. read it back: readSegment preserves the boundaries ----------
		Statement sel{att, tra, "select note from docs where id = 1"};
		sel.execute(tra);
		Blob readBack{att, tra, sel.getBlobId(0).value()};
		char seg[64];
		unsigned segLen, n = 0;
		while ((segLen = readBack.readSegment(std::span<char>{seg, sizeof seg - 1})) > 0)
		{
			seg[segLen] = 0;
			printf("  readSegment #%u: %2u bytes  \"%s\"\n", ++n, segLen, seg);
		}

		// -- 3. the blob describes itself ------------------------------------
		// fb-cpp surfaces the total length; for the full blh bookkeeping
		// (segment count, longest segment, type) drop to the raw handle.
		printf("Blob::getLength() = %u bytes\n", readBack.getLength());
		const unsigned char items[] = { isc_info_blob_num_segments,
			isc_info_blob_max_segment, isc_info_blob_type };
		unsigned char info[64];
		impl::StatusWrapper st{client};
		readBack.getHandle()->getInfo(&st, sizeof items, items, sizeof info, info);
		printf("raw getInfo via getHandle(): %ld segments, longest %ld, type %ld (0=segmented)\n",
			infoValue(info, sizeof info, isc_info_blob_num_segments),
			infoValue(info, sizeof info, isc_info_blob_max_segment),
			infoValue(info, sizeof info, isc_info_blob_type));
		readBack.close();

		// -- 4. the same bytes as a STREAM blob: no boundaries, but seek ----
		Blob stream{att, tra, BlobOptions().setType(BlobType::STREAM)};
		for (const char* s : segments)
			stream.write(std::span<const char>{s, strlen(s)});
		stream.close();
		Statement ins2{att, tra, "insert into docs (id, data) values (?, ?)"};
		ins2.set(0, 2);
		ins2.setBlobId(1, stream.getId());
		ins2.execute(tra);

		Statement sel2{att, tra, "select data from docs where id = 2"};
		sel2.execute(tra);
		// The open must ALSO say stream: without the BPB the blob is served
		// in segmented mode, and a seek() then silently returns garbage
		// (extra header bytes past the logical remainder).
		Blob stream2{att, tra, sel2.getBlobId(0).value(),
			BlobOptions().setType(BlobType::STREAM)};
		char buf[64];
		unsigned got = stream2.readSegment(std::span<char>{buf, sizeof buf - 1});
		buf[got] = 0;
		printf("\nstream twin: readSegment returned %u bytes in one chunk  \"%s\"\n", got, buf);
		stream2.seek(BlobSeekMode::FROM_BEGIN, 35);
		got = stream2.readSegment(std::span<char>{buf, sizeof buf - 1});
		buf[got] = 0;
		printf("seek(FROM_BEGIN, 35) then read: \"%s\"\n", buf);
		stream2.close();

		// -- 5. subtype text vs binary, from the catalog --------------------
		printf("\n-- column subtypes (RDB$FIELDS) --\n");
		Statement sub{att, tra,
			"select trim(rf.rdb$field_name), f.rdb$field_sub_type, trim(cs.rdb$character_set_name) "
			"from rdb$relation_fields rf "
			"join rdb$fields f on rf.rdb$field_source = f.rdb$field_name "
			"left join rdb$character_sets cs "
			"  on f.rdb$character_set_id = cs.rdb$character_set_id "
			"where rf.rdb$relation_name = 'DOCS' and f.rdb$field_type = 261 "
			"order by 1"};
		printf("FIELD SUBTYPE CHARSET\n----- ------- -------\n");
		for (bool more = sub.execute(tra); more; more = sub.fetchNext())
			printf("%-5s %-7s %s\n",
				sub.getString(0).value_or("<null>").c_str(),
				sub.getString(1).value_or("<null>").c_str(),
				sub.getString(2).value_or("<null>").c_str());

		// -- 6. BLOB_APPEND: build a blob in SQL without recopying ----------
		att.execute(tra, "insert into docs (id, note) values (3, "
			"blob_append(cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))");
		printf("\n-- BLOB_APPEND result --\n");
		Statement ba{att, tra,
			"select id, octet_length(note), char_length(note), cast(note as varchar(50)) "
			"from docs where id = 3"};
		ba.execute(tra);
		printf("ID OCTETS CHARS CONTENT\n-- ------ ----- -----------------\n");
		printf("%-2s %-6s %-5s %s\n",
			ba.getString(0).value_or("?").c_str(), ba.getString(1).value_or("?").c_str(),
			ba.getString(2).value_or("?").c_str(), ba.getString(3).value_or("?").c_str());

		tra.commit();
		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
