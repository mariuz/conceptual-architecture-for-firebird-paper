//
// catalog.js — companion to ../../catalog-bootstrap.md
//
// JavaScript twin of ../cpp/catalog.cpp: the self-describing catalog on a
// freshly created database — fixed relation ids, RDB$PAGES carrying its own
// pointer page (cross-checked against the hdr_PAGES word at byte 28 of the
// file), RDB$FORMATS empty for the 60 compiled-in system relations, then
// gaining rows as user DDL runs.  Run on the server machine:
//   node catalog.js
//
'use strict';

const fs = require('fs');
const { Firebird, DEFAULTS, attachOrCreate, s } = require('./common');

const DB = '/tmp/fbhandson/catalog_js.fdb';

const dropIfExists = () => new Promise(resolve =>
    Firebird.drop({ ...DEFAULTS, database: DB }, () => resolve()));  // ignore "no file"

(async () => {
    await dropIfExists();                       // a truly fresh database each run
    const conn = await attachOrCreate({ database: DB });

    console.log('-- 1. fixed relation ids (relations.h declaration order) --');
    for (const r of await conn.query(
        `select rdb$relation_id id, trim(rdb$relation_name) name
           from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1`))
        console.log(`  ${r.ID}  ${s(r.NAME)}`);

    console.log('\n-- 2. RDB$PAGES describing relation 0 (itself) and 6 (RDB$RELATIONS) --');
    console.log('  PAGE_NUMBER  RELATION_ID  SEQUENCE  PAGE_TYPE');
    for (const r of await conn.query(
        `select rdb$page_number pn, rdb$relation_id rid, rdb$page_sequence seq, rdb$page_type pt
           from rdb$pages where rdb$relation_id in (0, 6)
           order by rdb$relation_id, rdb$page_type, rdb$page_number`))
        console.log(`  ${String(r.PN).padStart(11)}  ${String(r.RID).padStart(11)}` +
            `  ${String(r.SEQ).padStart(8)}  ${String(r.PT).padStart(9)}`);

    // The anti-recursion anchor, straight from the file bytes.
    const fd = fs.openSync(DB, 'r');
    const word = Buffer.alloc(4);
    fs.readSync(fd, word, 0, 4, 28);
    fs.closeSync(fd);
    console.log(`\nhdr_PAGES (page 0, offset 28) = ${word.readUInt32LE(0)}` +
        '  <- matches the (relation 0, type 4) row above');

    console.log('\n-- 3. formats as code: zero stored formats, yet a full catalog --');
    const [c] = await conn.query(
        `select (select count(*) from rdb$formats) formats_rows,
                (select count(*) from rdb$relations where rdb$system_flag = 1) sys_relations,
                (select count(*) from rdb$relation_fields r join rdb$relations rel
                   on r.rdb$relation_name = rel.rdb$relation_name
                  and r.rdb$schema_name = rel.rdb$schema_name
                 where rel.rdb$system_flag = 1) sys_fields
           from rdb$database`);
    console.log(`  FORMATS_ROWS ${c.FORMATS_ROWS}, SYS_RELATIONS ${c.SYS_RELATIONS},` +
        ` SYS_FIELDS ${c.SYS_FIELDS}`);

    console.log('\n-- 4. user DDL writes formats into the catalog --');
    await conn.query('create table t1 (a integer)');
    await conn.query('alter table t1 add b varchar(10)');
    for (const r of await conn.query(
        `select rdb$relation_id rid, rdb$format fmt, octet_length(rdb$descriptor) bytes
           from rdb$formats order by rdb$relation_id, rdb$format`))
        console.log(`  relation ${r.RID}  format ${r.FMT}  descriptor ${r.BYTES} bytes`);

    await conn.detach();
    console.log('done.');
    process.exit(0);    // Firebird.drop() keeps its internal socket open, which
                        // would otherwise hold the event loop after we finish
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
