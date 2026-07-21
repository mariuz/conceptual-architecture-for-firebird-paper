/*
 *  ods_header.cpp — companion to ../../on-disk-structure.md
 *
 *  Reads a database file's header page (page 0) directly from disk and
 *  decodes it with the byte offsets that src/jrd/ods.h pins with
 *  static_asserts: page type, page size, ODS version, the hdr_PAGES
 *  bootstrap anchor, and the four transaction markers (Next/OIT/OAT/OST).
 *  Then it walks the whole file reading byte 0 of every page — the page
 *  type — to show "one file, many pages" as a census.
 *
 *  Steps: create/attach a scratch database through the server, ask
 *  MON$DATABASE for the server's own view of the same numbers, detach,
 *  then open the file and parse raw bytes.  Run it from the machine the
 *  server runs on (the server writes /tmp/fbhandson/ods.fdb; we read it).
 */

#include "fb_sample.h"
#include <cstdint>

using namespace fbsample;

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
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost//tmp/fbhandson/ods.fdb");
	const char* localFile = argOrDefault(argc, argv, 2, "/tmp/fbhandson/ods.fdb");

	try
	{
		// 1. Create (or reuse) the scratch database and generate a little
		//    transaction history so the TIP markers move off their floor.
		Db db;
		db.attachOrCreate(database);
		for (int i = 0; i < 3; ++i)
		{
			ITransaction* t = db.start();
			db.queryValue(t, "select 1 from rdb$database");
			t->commit(&db.status);
		}

		ITransaction* tra = db.start();
		printf("-- server's view (MON$DATABASE) --\n");
		Db::print(db.query(tra,
			"select mon$page_size, mon$ods_major, mon$ods_minor,"
			"       mon$oldest_transaction, mon$oldest_active,"
			"       mon$oldest_snapshot, mon$next_transaction "
			"from mon$database"));
		tra->commit(&db.status);
		db.att->detach(&db.status);
		db.att = nullptr;

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
	catch (const FbException& error)
	{
		return report(error);
	}
}
