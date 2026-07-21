//
// intl.js — companion sample for ../../internationalization.md.
//
// The JavaScript twin of ../cpp/intl.cpp, against the same table (run the
// C++ sample first or let this one create it).  node-firebird's `encoding`
// option is the connection charset (lc_ctype), so the same two-connection
// experiment works: with encoding UTF8 every column arrives transliterated
// into proper JS strings; with encoding NONE the server sends raw stored
// bytes and the driver decodes them byte-per-byte (latin1).  The WIN1252
// column then *accidentally* looks right (WIN1252 ~ latin1: E9 -> 'é')
// while the UTF8 column becomes the classic mojibake "CafÃ©" (C3 A9 read
// as two latin1 chars) — the passthrough behaviour behind issue #422 with
// NONE-charset databases (see common.js, which defaults to encoding NONE
// for employee.fdb).
//
// Run:  node intl.js
//
'use strict';

const { attachOrCreate, attach, s } = require('./common.js');

(async () => {
    const utf8 = await attachOrCreate({
        database: '/tmp/fbhandson/intl.fdb',
        encoding: 'UTF8',
    });
    const one = async (c, sql) => Object.values((await c.query(sql))[0])[0];
    try {
        // Idempotent setup — same table as the C++ sample.
        try { await utf8.query('DROP TABLE t'); } catch (e) {}
        await utf8.query(
            'CREATE TABLE t ('
            + ' name_ci_ai VARCHAR(30) CHARACTER SET UTF8 COLLATE UNICODE_CI_AI,'
            + ' name_bin   VARCHAR(30) CHARACTER SET UTF8 COLLATE UCS_BASIC,'
            + ' name_win   VARCHAR(30) CHARACTER SET WIN1252)');
        for (const v of ['Café', 'CAFE', 'cafe'])
            await utf8.query('INSERT INTO t VALUES (?,?,?)', [v, v, v]);

        console.log("matches for 'cafe' via UNICODE_CI_AI :",
            await one(utf8, "SELECT COUNT(*) FROM t WHERE name_ci_ai = 'cafe'"));
        console.log("matches for 'cafe' via UCS_BASIC     :",
            await one(utf8, "SELECT COUNT(*) FROM t WHERE name_bin = 'cafe'"));
        console.log("UPPER('café èñ ß')                   :",
            s(await one(utf8, "SELECT UPPER('café èñ ß') FROM RDB$DATABASE")));

        // The same row over two connection charsets (lc_ctype).
        const cp = v => [...v].map(c => c.codePointAt(0).toString(16)).join(' ');
        const both = async (conn, label) => {
            const r = (await conn.query(
                "SELECT name_win, name_bin FROM t WHERE name_bin = 'Café'"))[0];
            console.log(`${label} name_win=${JSON.stringify(r.NAME_WIN)}`
                + ` [${cp(r.NAME_WIN)}]  name_bin=${JSON.stringify(r.NAME_BIN)}`
                + ` [${cp(r.NAME_BIN)}]`);
        };
        console.log('');
        await both(utf8, 'encoding UTF8 :');
        const none = await attach({ database: '/tmp/fbhandson/intl.fdb', encoding: 'NONE' });
        try {
            await both(none, 'encoding NONE :');
        } finally {
            await none.detach();
        }
        console.log(' -> NONE passthrough (codepoints in brackets): the WIN1252 byte E9'
            + " accidentally decodes\n    as latin1 'é', while the raw UTF8 bytes C3 A9"
            + ' become the mojibake pair Ã © —\n    no transliteration happened on the server.');
    } finally {
        await utf8.detach();
    }
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
