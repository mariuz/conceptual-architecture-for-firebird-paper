//
// migration.js — companion sample for ../../migration-and-interoperability.md
//
// The other half of the type-mapping probe in ../cpp/migration.cpp: what a
// generic driver actually MATERIALIZES for each Firebird-specific type.
// node-firebird is a good specimen — a pure-JavaScript driver that must map
// every wire type onto JavaScript's small set of value types, exactly the
// downcasting problem every migration faces.  Each column is probed on its
// own, because some types do not survive the trip at all — which is the
// honest result, not a failure of the sample.
//
'use strict';

const { attachOrCreate, s } = require('./common');

const DB = '/tmp/fbhandson/migration_js.fdb';

const describe = v => {
    if (v === null) return '<null>';
    if (Buffer.isBuffer(v)) return `Buffer(${v.length})  0x${v.toString('hex')}`;
    if (v instanceof Date) return `Date  ${v.toISOString()}`;
    return `${typeof v}  ${v}`;
};

(async () => {
    const conn = await attachOrCreate({ database: DB, encoding: 'UTF8' });

    try { await conn.query('DROP TABLE TYPE_PROBE'); } catch (e) {}
    await conn.query(
        `CREATE TABLE TYPE_PROBE (
           C_INT128 INT128,
           C_NUM    NUMERIC(38,8),
           C_DEC    DECFLOAT(34),
           C_TSTZ   TIMESTAMP WITH TIME ZONE,
           C_BOOL   BOOLEAN,
           C_UUID   CHAR(16) CHARACTER SET OCTETS,
           C_VC     VARCHAR(20))`);
    await conn.query(
        `INSERT INTO TYPE_PROBE VALUES (
           170141183460469231731687303715884105727,
           123456789012345678901234567890.12345678,
           1.234567890123456789012345678901234E+10,
           TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',
           TRUE, GEN_UUID(), 'naïve ütf8 text')`);

    console.log('what node-firebird materializes, column by column:\n');
    for (const col of ['C_INT128', 'C_NUM', 'C_DEC', 'C_TSTZ', 'C_BOOL', 'C_UUID', 'C_VC']) {
        try {
            const row = (await conn.query(`SELECT ${col} V FROM TYPE_PROBE`))[0];
            console.log(`  ${col.padEnd(8)} -> ${describe(row.V)}`);
        } catch (e) {
            console.log(`  ${col.padEnd(8)} -> ERROR: ${e.message.split('\n')[0]}`);
        }
    }

    console.log('\nthe fallback every migration has: engine-rendered text (CAST):\n');
    const txt = (await conn.query(
        `SELECT CAST(C_INT128 AS VARCHAR(50)) C_INT128,
                CAST(C_DEC    AS VARCHAR(50)) C_DEC,
                CAST(C_TSTZ   AS VARCHAR(60)) C_TSTZ,
                UUID_TO_CHAR(C_UUID)          C_UUID
         FROM TYPE_PROBE`))[0];
    for (const [name, value] of Object.entries(txt))
        console.log(`  ${name.padEnd(8)} = ${s(value)}`);

    await conn.detach();
    console.log('\ndone.');
    process.exit(0);
})().catch(e => { console.error('FAILED:', e.message || e); process.exit(1); });
