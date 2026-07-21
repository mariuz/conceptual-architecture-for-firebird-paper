//
// ods_header.js — companion to ../../on-disk-structure.md
//
// The JavaScript twin of ../cpp/ods_header.cpp, and a natural fit for Node:
// Buffer.readUInt16LE / readBigUInt64LE parse the header page (page 0)
// directly at the byte offsets src/jrd/ods.h pins with static_asserts.
// Creates a scratch database through the server, compares MON$DATABASE's
// view with the raw bytes, then runs a page-type census over the file.
// Run on the server machine: node ods_header.js
//
'use strict';

const fs = require('fs');
const { attachOrCreate, s } = require('./common');

const DB_REMOTE = '/tmp/fbhandson/ods_js.fdb';
const DB_LOCAL = '/tmp/fbhandson/ods_js.fdb';

const PAGE_TYPES = ['?', 'pag_header', 'pag_pages (PIP)', 'pag_transactions (TIP)',
    'pag_pointer', 'pag_data', 'pag_root', 'pag_index (b-tree)', 'pag_blob',
    'pag_ids (generators)', 'pag_scns'];

(async () => {
    // 1. Create/attach through the server; a few commits move the TIP markers.
    const conn = await attachOrCreate({ database: DB_REMOTE });
    for (let i = 0; i < 3; i++)
        await conn.query('select 1 from rdb$database');

    const [mon] = await conn.query(
        `select mon$page_size ps, mon$ods_major omaj, mon$ods_minor omin,
                mon$oldest_transaction oit, mon$oldest_active oat,
                mon$oldest_snapshot ost, mon$next_transaction nxt
           from mon$database`);
    console.log("-- server's view (MON$DATABASE) --");
    console.log(`page size ${mon.PS}, ODS ${mon.OMAJ}.${mon.OMIN},` +
        ` OIT ${mon.OIT}, OAT ${mon.OAT}, OST ${mon.OST}, next ${mon.NXT}`);
    await conn.detach();

    // 2. The same facts from the bytes.  struct header_page is 152 bytes.
    const file = fs.readFileSync(DB_LOCAL);        // whole file; census needs it anyway
    const h = file.subarray(0, 152);

    const odsRaw = h.readUInt16LE(18);
    const pageSize = h.readUInt16LE(16);
    const guid = (b) => (`{${b.readUInt32LE(0).toString(16).padStart(8, '0')}-` +
        `${b.readUInt16LE(4).toString(16).padStart(4, '0')}-` +
        `${b.readUInt16LE(6).toString(16).padStart(4, '0')}-` +
        `${b.subarray(8, 10).toString('hex')}-` +
        `${b.subarray(10, 16).toString('hex')}}`).toUpperCase();

    console.log(`\n-- header page, parsed from ${DB_LOCAL} (offsets per ods.h) --`);
    console.log(`pag_type      @0   = ${h[0]} (${PAGE_TYPES[h[0]]})`);
    console.log(`hdr_page_size @16  = ${pageSize}`);
    console.log(`hdr_ods_version @18 = 0x${odsRaw.toString(16)} -> ODS ${odsRaw & 0x7fff}` +
        ` (FIREBIRD flag ${odsRaw & 0x8000 ? 'set' : 'clear'}), minor @20 = ${h.readUInt16LE(20)}`);
    const flags = h.readUInt16LE(22);
    console.log(`hdr_flags     @22  = 0x${flags.toString(16)}` +
        `${flags & 0x2 ? ' force_write' : ''}${flags & 0x10 ? ' SQL_dialect_3' : ''}`);
    console.log(`hdr_PAGES     @28  = ${h.readUInt32LE(28)}` +
        '   <- pointer page of RDB$PAGES (catalog bootstrap anchor)');
    console.log(`hdr_next_transaction   @40 = ${h.readBigUInt64LE(40)}`);
    console.log(`hdr_oldest_transaction @48 = ${h.readBigUInt64LE(48)} (OIT)`);
    console.log(`hdr_oldest_active      @56 = ${h.readBigUInt64LE(56)} (OAT)`);
    console.log(`hdr_oldest_snapshot    @64 = ${h.readBigUInt64LE(64)} (OST)`);
    console.log(`hdr_guid      @84  = ${guid(h.subarray(84, 100))}`);

    // 3. Page-type census: byte 0 of every page.
    const counts = new Map();
    for (let off = 0; off < file.length; off += pageSize)
        counts.set(file[off], (counts.get(file[off]) || 0) + 1);
    console.log(`\n-- page-type census: ${file.length / pageSize} pages of ${pageSize} bytes --`);
    for (const t of [...counts.keys()].sort((a, b) => a - b))
        console.log(`  type ${String(t).padStart(2)}  ${PAGE_TYPES[t].padEnd(22)} ${String(counts.get(t)).padStart(5)}`);
    console.log('done.');
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
