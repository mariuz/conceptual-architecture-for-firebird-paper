//
// ha.js — companion sample for ../../high-availability.md
//
// The same shadow exercise as ../cpp/ha.cpp through node-firebird: CREATE
// SHADOW on a scratch database, watch the mirror file appear and grow in
// lock-step with the main file (fs.statSync works because server and sample
// share a host here), then DROP SHADOW.  Everything is ordinary client SQL —
// no config files, no service manager.
//
'use strict';

const fs = require('fs');
const { attachOrCreate, s } = require('./common');

const MAIN = '/tmp/fbhandson/ha_js.fdb';       // server-side paths
const SHAD = '/tmp/fbhandson/ha_js.shd';

const size = p => { try { return fs.statSync(p).size; } catch (e) { return -1; } };
const show = when => console.log(
    `${when.padEnd(24)} main = ${size(MAIN)} bytes, shadow = ${size(SHAD)} bytes`);

(async () => {
    const conn = await attachOrCreate({ database: MAIN, encoding: 'UTF8' });

    // Idempotent cleanup from earlier runs.
    try { await conn.query('DROP SHADOW 1 DELETE FILE'); } catch (e) {}
    try { await conn.query('DROP TABLE HA_LOG'); } catch (e) {}
    await conn.query('CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))');

    // 1. Create the synchronous page-level mirror.
    await conn.query(`CREATE SHADOW 1 '${SHAD}'`);
    console.log('CREATE SHADOW 1 done\n');

    const files = await conn.query(
        'SELECT RDB$FILE_NAME F, RDB$SHADOW_NUMBER N, RDB$FILE_FLAGS FL FROM RDB$FILES');
    for (const r of files)
        console.log(`RDB$FILES: shadow ${r.N} -> ${s(r.F)} (flags ${r.FL})`);
    console.log('');
    show('after CREATE SHADOW:');

    // 2. Write load: every page write now goes to both files.
    await conn.query(
        `EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN
           WHILE (I < 5000) DO BEGIN
             INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1;
           END
         END`);
    show('after 5000 inserts:');

    // 3. Retire the mirror.
    await conn.query('DROP SHADOW 1 DELETE FILE');
    console.log('\nDROP SHADOW 1 DELETE FILE done');
    show('after DROP SHADOW:');

    const left = await conn.query('SELECT COUNT(*) N FROM RDB$FILES');
    console.log(`\nRDB$FILES rows left: ${left[0].N}`);
    await conn.detach();
    console.log('done.');
    process.exit(0);
})().catch(e => { console.error('FAILED:', e.message || e); process.exit(1); });
