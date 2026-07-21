//
// metadata_cache.js — companion to ../../metadata-cache.md
//
// The JS twin of ../cpp/metadata_cache.cpp: the metadata cache's visibility
// rule observed through two wire-protocol attachments.  An uncommitted
// ALTER is visible only to its own transaction; a committed one is visible
// immediately even to a statement prepared inside an older, still-open
// SNAPSHOT transaction (metadata is read-committed, records are not); two
// uncommitted DDLs collide with the engine's verbatim newVersion error; and
// RDB$FORMATS keeps one row per shape the table has lived through.
//
// Run: node metadata_cache.js
//
'use strict';

const { attachOrCreate, attach, ISOLATION, s } = require('./common');

const DB = '/tmp/fbhandson/mdc_js.fdb';

async function show(who, q, promise) {
    try {
        const rows = await promise;
        const v = rows && rows[0] ? Object.values(rows[0])[0] : '(ok)';
        console.log(`${who}: ${q} -> ${v === null ? '<null>' : s(v)}`);
    } catch (e) {
        console.log(`${who}: ${q} -> ERROR: ${e.message.replace(/\n/g, ' ')}`);
    }
}

(async () => {
    const A = await attachOrCreate({ database: DB });
    const B = await attach({ database: DB });
    await A.query('recreate table t (a integer)');
    await A.query('insert into t values (1)');

    console.log('== 1. uncommitted ALTER: visible to creator only ==');
    const aDdl = await A.transaction();          // A's DDL stays uncommitted
    await aDdl.query('alter table t add e integer');
    await show('A (same tx)  ', 'select e from t', aDdl.query('select e from t'));
    await show('B            ', 'select e from t', B.query('select e from t'));

    console.log("\n== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==");
    const bSnap = await B.transaction(ISOLATION.SNAPSHOT);
    await show('B (snapshot) ', 'select count(*) from t', bSnap.query('select count(*) from t'));
    await aDdl.commit();                         // E becomes committed
    await A.query('alter table t add d integer');// D committed after B's snapshot
    await show('B (same  tx) ', 'select d from t', bSnap.query('select d from t'));
    console.log('   (metadata is read-committed; the statement was prepared\n' +
        "    against the version chain's current head)");
    await bSnap.commit();

    console.log('\n== 3. two uncommitted DDLs on one object ==');
    const aDdl2 = await A.transaction();
    await aDdl2.query('alter table t add f integer');
    const bDdl = await B.transaction();
    await show('B            ', 'alter table t add g integer',
        bDdl.query('alter table t add g integer'));
    await bDdl.rollback();
    await aDdl2.rollback();                      // F vanishes with the rollback

    console.log('\n== 4. RDB$FORMATS after the committed DDL ==');
    const [f] = await A.query(
        `select count(*) c from rdb$formats f
           join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id
          where r.rdb$relation_name = 'T'`);
    console.log(`formats stored for T: ${f.C}`);
    const [row] = await A.query('select a, e, d from t');
    console.log(`row written under shape 1 decodes as: a=${row.A}, e=${row.E}, d=${row.D}`);

    await B.detach();
    await A.detach();
    console.log('done.');
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
