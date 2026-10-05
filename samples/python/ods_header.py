#
# ods_header.py - the header page read straight from the file (Python twin
# of ../cpp/ods_header.cpp; see ../../on-disk-structure.md).
#
# Creates (or reuses) a scratch database through the server, commits a few
# transactions so the TIP markers move, then reads the same facts three
# ways: MON$DATABASE (SQL), the database info API (no SQL at all), and the
# raw bytes of page 0 decoded at the offsets src/jrd/ods.h pins with
# static_asserts.  It ends with a page-type census: byte 0 of every page.
#
# firebird-driver's Connection.info decodes isc_database_info into typed
# properties - ods_version / ods_minor_version, page_size, pages_allocated,
# and, unlike fbintf, the 64-bit transaction markers oit / oat / ost /
# next_transaction (info tags 104-107) - plus get_info(DbInfoCode.DB_GUID).
# The file half is struct.unpack_from: no driver can abstract the format.
# Run it on the server machine (the server writes the file; we read it - the
# reader needs the firebird group, e.g. `sg firebird -c ...`).
#
# Run:  python3 ods_header.py [database [local-file]]
#
import struct
import sys

from firebird.driver import DbInfoCode

from fbsample import attach_or_create, db_path, query, run

PAGE_TYPES = ('undefined', 'pag_header', 'pag_pages (PIP)', 'pag_transactions (TIP)',
              'pag_pointer', 'pag_data', 'pag_root', 'pag_index (b-tree)', 'pag_blob',
              'pag_ids (generators)', 'pag_scns')


def main():
    path = db_path('ods')
    local = sys.argv[2] if len(sys.argv) > 2 else path

    # 1. The server's views, while attached.
    with attach_or_create(path) as db:
        for _ in range(3):
            query(db, 'select 1 from rdb$database')
            db.commit()
        cols = ('mon$page_size, mon$ods_major, mon$ods_minor, mon$oldest_transaction,'
                ' mon$oldest_active, mon$oldest_snapshot, mon$next_transaction')
        print('-- server\'s view (MON$DATABASE) --')
        print('page_size ods_major ods_minor oit oat ost next =',
              *query(db, f'select {cols} from mon$database')[0])
        db.commit()
        i = db.info
        print('\n-- the same through the info API (Connection.info) --')
        print(f'ods {i.ods_version}.{i.ods_minor_version}  page_size {i.page_size}'
              f'  pages_allocated {i.pages_allocated}')
        print(f'oit {i.oit}  oat {i.oat}  ost {i.ost}  next {i.next_transaction}'
              f'  guid {i.get_info(DbInfoCode.DB_GUID)}')

    # 2. The same facts straight from the bytes on disk.
    with open(local, 'rb') as f:
        h = f.read(152)                              # sizeof(Ods::header_page)
        u16 = lambda o: struct.unpack_from('<H', h, o)[0]   # noqa: E731
        u32 = lambda o: struct.unpack_from('<I', h, o)[0]   # noqa: E731
        u64 = lambda o: struct.unpack_from('<Q', h, o)[0]   # noqa: E731
        print(f'\n-- header page, parsed from {local} (offsets per ods.h) --')
        print(f'pag_type      @0   = {h[0]} ({PAGE_TYPES[h[0]]})')
        page_size, ods_raw, flags = u16(16), u16(18), u16(22)
        print(f'hdr_page_size @16  = {page_size}')
        print(f'hdr_ods_version @18 = 0x{ods_raw:04x} -> ODS {ods_raw & 0x7fff} (FIREBIRD flag '
              f'0x8000 {"set" if ods_raw & 0x8000 else "clear"}), minor @20 = {u16(20)}')
        names = ' '.join(n for bit, n in ((0x2, 'force_write'), (0x8, 'no_reserve'),
                                          (0x10, 'SQL_dialect_3')) if flags & bit)
        print(f'hdr_flags     @22  = 0x{flags:02x} ({names})')
        print(f'hdr_PAGES     @28  = {u32(28)}   <- pointer page of RDB$PAGES')
        print(f'hdr_next_transaction   @40 = {u64(40)}')
        print(f'hdr_oldest_transaction @48 = {u64(48)} (OIT)')
        print(f'hdr_oldest_active      @56 = {u64(56)} (OAT)')
        print(f'hdr_oldest_snapshot    @64 = {u64(64)} (OST)')
        a, b, c = struct.unpack_from('<IHH', h, 84)
        g = h[92:100]
        print(f'hdr_guid      @84  = {{{a:08X}-{b:04X}-{c:04X}-{g[:2].hex().upper()}-'
              f'{g[2:].hex().upper()}}}')

        # 3. Page-type census: byte 0 of every page in the file.
        counts = [0] * 11
        pages = 0
        while True:
            f.seek(pages * page_size)
            t = f.read(1)
            if not t:
                break
            counts[t[0] if t[0] <= 10 else 0] += 1
            pages += 1

    print(f'\n-- page-type census: {pages} pages of {page_size} bytes --')
    for t in range(11):
        if counts[t]:
            print(f'  type {t:2d}  {PAGE_TYPES[t]:<22} {counts[t]:5d}')
    print('done.')


if __name__ == '__main__':
    run(main)
