/*
 *  ods_header.cpp (fb-cpp) — companion to ../../on-disk-structure.md
 *
 *  The fb-cpp twin of ../cpp/ods_header.cpp.  The database side — create
 *  a scratch database, roll a little transaction history, ask
 *  MON$DATABASE for the server's view of the header numbers — shrinks to
 *  a few typed calls; the raw half stays exactly what it was, plain C++
 *  reading bytes at the offsets src/jrd/ods.h pins with static_asserts.
 *  No wrapper can abstract the file format away, which is this document's
 *  point: the file IS the database.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_ods_header [database] [localFile]
 */

#include "fbcpp_sample.h"
#include <cstdint>
#include <cstring>

using namespace fbcpp;
using namespace fbcpp_sample;

static uint16_t u16(const unsigned char* b, size_t o) { uint16_t v; memcpy(&v, b + o, 2); return v; }
static uint32_t u32(const unsigned char* b, size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static uint64_t u64(const unsigned char* b, size_t o) { uint64_t v; memcpy(&v, b + o, 8); return v; }

static const char* pageTypeName(int t)
{
	static const char* names[] = { "undefined", "pag_header", "pag_pages (PIP)",
		"pag_transactions (TIP)", "pag_pointer", "pag_data", "pag_root",
		"pag_index (b-tree)", "pag_blob", "pag_ids (generators)", "pag_scns" };
	return (t >= 0 && t <= 10) ? names[t] : "???";
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/ods_fbcpp.fdb");
	const char* localFile = argOrDefault(argc, argv, 2, "/tmp/fbhandson/ods_fbcpp.fdb");

	try
	{
		// 1. Create (or reuse) the scratch database and generate a little
		//    transaction history so the TIP markers move off their floor.
		Client client{"fbclient"};
		{
			Attachment att = attachOrCreate(client, database);
			for (int i = 0; i < 3; ++i)
			{
				Transaction t{att};
				Statement s{att, t, "select 1 from rdb$database"};
				s.execute(t);
				t.commit();
			}

			Transaction tra{att};
			Statement mon{att, tra,
				"select mon$page_size, mon$ods_major, mon$ods_minor,"
				"       mon$oldest_transaction, mon$oldest_active,"
				"       mon$oldest_snapshot, mon$next_transaction "
				"from mon$database"};
			mon.execute(tra);
			printf("-- server's view (MON$DATABASE) --\n");
			printf("page_size %s, ODS %s.%s, OIT %s, OAT %s, OST %s, next %s\n",
				mon.getString(0).value_or("?").c_str(), mon.getString(1).value_or("?").c_str(),
				mon.getString(2).value_or("?").c_str(), mon.getString(3).value_or("?").c_str(),
				mon.getString(4).value_or("?").c_str(), mon.getString(5).value_or("?").c_str(),
				mon.getString(6).value_or("?").c_str());
			tra.commit();
		}										// RAII: attachment detaches here

		// 2. Now the same facts straight from the bytes on disk.
		FILE* f = fopen(localFile, "rb");
		if (!f)
			{ perror(localFile); return 1; }

		unsigned char h[152];					// sizeof(Ods::header_page)
		if (fread(h, 1, sizeof h, f) != sizeof h)
			{ fprintf(stderr, "short read\n"); return 1; }

		printf("\n-- header page, parsed from %s (offsets per ods.h) --\n", localFile);
		printf("pag_type      @0   = %u (%s)\n", h[0], pageTypeName(h[0]));
		printf("pag_flags     @1   = %u\n", h[1]);

		const unsigned pageSize = u16(h, 16);
		const uint16_t odsRaw = u16(h, 18);
		printf("hdr_page_size @16  = %u\n", pageSize);
		printf("hdr_ods_version @18 = 0x%04x -> ODS %u (FIREBIRD flag 0x8000 %s), minor @20 = %u\n",
			odsRaw, odsRaw & 0x7fff, (odsRaw & 0x8000) ? "set" : "clear", u16(h, 20));

		const uint16_t flags = u16(h, 22);
		printf("hdr_flags     @22  = 0x%02x (%s%s%s)\n", flags,
			(flags & 0x2) ? "force_write " : "", (flags & 0x8) ? "no_reserve " : "",
			(flags & 0x10) ? "SQL_dialect_3" : "");
		printf("hdr_PAGES     @28  = %u   <- pointer page of RDB$PAGES (catalog bootstrap anchor)\n",
			u32(h, 28));
		printf("hdr_next_transaction   @40 = %llu\n", (unsigned long long) u64(h, 40));
		printf("hdr_oldest_transaction @48 = %llu (OIT)\n", (unsigned long long) u64(h, 48));
		printf("hdr_oldest_active      @56 = %llu (OAT)\n", (unsigned long long) u64(h, 56));
		printf("hdr_oldest_snapshot    @64 = %llu (OST)\n", (unsigned long long) u64(h, 64));

		const unsigned char* g = h + 84;		// hdr_guid: Win32 GUID layout
		printf("hdr_guid      @84  = {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
			u32(g, 0), u16(g, 4), u16(g, 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);

		// 3. Page-type census: byte 0 of every page in the file.
		unsigned counts[11] = {};
		unsigned long pageNo = 0;
		for (;; ++pageNo)
		{
			if (fseek(f, (long) (pageNo * (unsigned long) pageSize), SEEK_SET) != 0)
				break;
			int c = fgetc(f);
			if (c == EOF)
				break;
			counts[(c >= 0 && c <= 10) ? c : 0]++;
		}
		fclose(f);

		printf("\n-- page-type census: %lu pages of %u bytes --\n", pageNo, pageSize);
		for (int t = 1; t <= 10; ++t)
			if (counts[t])
				printf("  type %2d  %-22s %5u\n", t, pageTypeName(t), counts[t]);
		printf("done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
