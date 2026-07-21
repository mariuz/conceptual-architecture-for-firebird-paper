//
// backup.js — companion sample for ../../backup-and-recovery.md
//
// The same gbak backup + restore round trip as ../cpp/backup.cpp, but through
// node-firebird's pure-JavaScript Services API client: attach() with
// { manager: true } speaks op_service_attach on the wire and returns a
// ServiceManager whose backup()/restore() build the same SPB action blocks
// fbsvcmgr does, streaming gbak's verbose log back as a Readable stream.
//
'use strict';

const { Firebird, DEFAULTS, attach, attachOrCreate, s } = require('./common');

const DB = '/tmp/fbhandson/backup_js.fdb';       // server-side paths
const FBK = '/tmp/fbhandson/backup_js.fbk';
const DB_RESTORED = '/tmp/fbhandson/backup_js_restored.fdb';

// Attach to service_mgr; the callback yields a ServiceManager, not a Database.
function attachServiceManager() {
    return new Promise((resolve, reject) =>
        Firebird.attach({ ...DEFAULTS, manager: true }, (err, svc) =>
            err ? reject(err) : resolve(svc)));
}

// Run one service action and print its verbose output line by line.
function streamLines(stream, prefix) {
    return new Promise((resolve, reject) => {
        stream.on('data', line => console.log(prefix + s(line)));
        stream.on('end', resolve);
        stream.on('error', reject);
    });
}

(async () => {
    // 1. scratch source database with a table and rows
    const conn = await attachOrCreate({ database: DB, encoding: 'UTF8' });
    try { await conn.query('DROP TABLE BR_ITEMS'); } catch (e) {}
    await conn.query('CREATE TABLE BR_ITEMS (ID INT NOT NULL PRIMARY KEY, NAME VARCHAR(30))');
    for (const [id, name] of [[1, 'alpha'], [2, 'beta'], [3, 'gamma']])
        await conn.query('INSERT INTO BR_ITEMS VALUES (?, ?)', [id, name]);
    await conn.detach();
    console.log('source ready: BR_ITEMS with 3 rows');

    // 2. gbak backup, then restore, through the service manager
    const svc = await attachServiceManager();

    console.log(`\n== backup: ${DB} -> ${FBK} ==`);
    await streamLines(
        await svc.backupAsync({ database: DB, files: FBK, verbose: true }),
        '  gbak> ');

    console.log(`\n== restore: ${FBK} -> ${DB_RESTORED} ==`);
    await streamLines(
        await svc.restoreAsync({ files: FBK, database: DB_RESTORED,
                                 replace: true, pagesize: 8192, verbose: true }),
        '  gbak> ');

    await svc.detachAsync();

    // 3. prove the restored copy has the data
    const check = await attach({ database: DB_RESTORED, encoding: 'UTF8' });
    const rows = await check.query('SELECT COUNT(*) N, MAX(NAME) MX FROM BR_ITEMS');
    console.log(`\nrestored database says: ${rows[0].N} rows, max name = ${s(rows[0].MX)}`);
    await check.detach();
    console.log('done.');
    process.exit(0);
})().catch(e => { console.error('FAILED:', e.message || e); process.exit(1); });
