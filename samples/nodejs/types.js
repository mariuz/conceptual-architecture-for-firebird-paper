//
// types.js — companion sample for ../../sql-dialect-and-types.md.
//
// The JavaScript twin of ../cpp/types.cpp: reads the same showcase table
// (run the C++ sample once first, or any of the DDL below) and reports,
// type by type, what node-firebird 2.11 actually delivers.  The point is
// honesty about driver coverage: a pure-JS wire-protocol driver must
// implement every wire type itself, and the FB4 types are where it shows.
//
//   BOOLEAN                  -> native JS boolean          (correct)
//   TIMESTAMP WITH TIME ZONE -> JS Date, instant correct,  (zone name lost)
//   VARCHAR (domain)         -> string                     (correct)
//   INT128                   -> WRONG value (mis-scaled)   (driver gap)
//   DECFLOAT(34)             -> -804 error, cannot fetch   (driver gap)
//
// The workaround for the last two is to make the server do the formatting:
// CAST(... AS VARCHAR(...)) — the same engine text conversion the C++
// helper uses for every column.
//
// Run:  node types.js     (see ../../sql-dialect-and-types.md, Hands-on)
//
'use strict';

const { attachOrCreate, s } = require('./common.js');

const show = v =>
    `${JSON.stringify(v instanceof Date ? v.toISOString() : Buffer.isBuffer(v) ? s(v) : v)}` +
    `  [${v === null ? 'null' : v instanceof Date ? 'Date' : typeof v}]`;

(async () => {
    const db = await attachOrCreate({
        database: '/tmp/fbhandson/types.fdb',
        encoding: 'UTF8',               // scratch DB is UTF8, unlike employee
    });
    try {
        // Idempotent setup (same table as the C++ sample).
        try { await db.query('DROP TABLE showcase'); } catch (e) {}
        try { await db.query('DROP DOMAIN d_email'); } catch (e) {}
        await db.query(
            "CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')");
        await db.query(
            'CREATE TABLE showcase (flag BOOLEAN, big INT128,' +
            ' money DECFLOAT(34), born TIMESTAMP WITH TIME ZONE, mail d_email)');
        await db.query(
            'INSERT INTO showcase VALUES (TRUE,' +
            ' 170141183460469231731687303715884105727, 0.1,' +
            " TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest'," +
            " 'user@example.com')");

        // 1. What the driver decodes natively, column by column.
        for (const col of ['flag', 'born', 'mail', 'big', 'money']) {
            try {
                const rows = await db.query(`SELECT ${col} FROM showcase`);
                console.log(`${col.padEnd(6)} -> ${show(Object.values(rows[0])[0])}`);
            } catch (e) {
                console.log(`${col.padEnd(6)} -> FETCH FAILED: ${e.message}`);
            }
        }

        // 2. The INT128 value is not just oddly typed — it is wrong.
        //    Compare the raw fetch above with the server-side cast:
        const truth = await db.query(
            'SELECT CAST(big AS VARCHAR(45)) AS big,' +
            ' CAST(money AS VARCHAR(45)) AS money FROM showcase');
        console.log('\nserver-side CAST(... AS VARCHAR) — the reliable route:');
        console.log(`big    -> ${show(s(truth[0].BIG))}`);
        console.log(`money  -> ${show(s(truth[0].MONEY))}`);

        // 3. The domain's CHECK constraint fires over the wire too.
        try {
            await db.query("INSERT INTO showcase (mail) VALUES ('nope')");
            console.log('BUG: domain CHECK did not fire');
        } catch (e) {
            console.log(`\ndomain CHECK rejected 'nope': ${e.message.split('\n')[0]}`);
        }
    } finally {
        await db.detach();
    }
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
