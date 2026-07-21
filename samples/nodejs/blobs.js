//
// blobs.js — companion to ../../blob-handling.md
//
// BLOB streaming through node-firebird's wire-protocol driver.  Writing: a
// string or Buffer parameter becomes op_create_blob2 + segment batches (1 KB
// chunks by default).  Reading: a blob column arrives as a *function*; call
// it and you get an EventEmitter delivering the value chunk by chunk — the
// driver's op_open_blob / op_get_segment loop made visible.
//
// (node-firebird 2.x also has a maxInlineBlobSize option for the FB5+
// op_inline_blob optimization — ship small blobs with the row — but against
// this Firebird 6 server enabling it hangs the query, so the sample sticks
// to the classic segment stream.  See ../../blob-handling.md.)
//
// Run: node blobs.js
//
'use strict';

const { attachOrCreate, s } = require('./common');

const DB = '/tmp/fbhandson/blobs_js.fdb';

// Call a fetched blob-column function, resolve with the array of data chunks.
const readBlob = (colFn) => new Promise((resolve, reject) =>
    colFn((err, name, e) => {
        if (err) return reject(err);
        const chunks = [];
        e.on('data', (c) => chunks.push(c));
        e.on('error', reject);
        e.on('end', () => resolve(chunks));
    }));

const show = (label, chunks) => {
    const total = chunks.reduce((n, c) => n + c.length, 0);
    console.log(`${label}: ${chunks.length} chunk(s) [${chunks.map(c => c.length).join(', ')}]` +
        ` = ${total} bytes`);
};

(async () => {
    const conn = await attachOrCreate({ database: DB });
    await conn.query('recreate table docs (' +
        ' id integer primary key,' +
        ' note blob sub_type text character set utf8,' +
        ' data blob sub_type binary)');

    // 1. Write: a ~5 KB string and a binary Buffer as ordinary parameters.
    const text = 'the quick brown fox jumps over the lazy dog. '.repeat(110); // 4950 bytes
    const bin = Buffer.from(Array.from({ length: 256 }, (_, i) => i));
    await conn.query('insert into docs (id, note, data) values (?, ?, ?)', [1, text, bin]);
    console.log(`wrote: ${text.length}-byte text blob, ${bin.length}-byte binary blob`);

    // 2. Read back: the column value is a function -> EventEmitter of chunks.
    const [row] = await conn.query('select note, data from docs where id = 1');
    show('note (segment stream)', await readBlob(row.NOTE));
    const binChunks = await readBlob(row.DATA);
    show('data (segment stream)', binChunks);
    console.log(`binary round-trip intact: ${Buffer.concat(binChunks).equals(bin)}`);

    // 3. BLOB_APPEND builds a blob server-side without recopying.
    await conn.query("insert into docs (id, note) values (2, " +
        "blob_append(cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))");
    const [ba] = await conn.query(
        'select octet_length(note) olen, cast(note as varchar(50)) v from docs where id = 2');
    console.log(`BLOB_APPEND: ${ba.OLEN} bytes -> "${s(ba.V)}"`);
    await conn.detach();

    console.log('done.');
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
