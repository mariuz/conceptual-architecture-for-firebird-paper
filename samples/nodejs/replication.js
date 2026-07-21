//
// replication.js — companion sample for ../../replication-architecture.md
//
// Same client-visible half of replication as ../cpp/replication.cpp: the
// publication DDL and the system tables that record it.  No replication.conf,
// no restart — this is exactly the part of the walk-through a driver can do.
//
'use strict';

const { attachOrCreate, s } = require('./common');

const DB = '/tmp/fbhandson/replication_js.fdb';

(async () => {
    const conn = await attachOrCreate({ database: DB, encoding: 'UTF8' });

    const pubState = async when => {
        const pubs = await conn.query(
            'SELECT RDB$PUBLICATION_NAME P, RDB$ACTIVE_FLAG A, RDB$AUTO_ENABLE AE FROM RDB$PUBLICATIONS');
        const tabs = await conn.query(
            'SELECT RDB$TABLE_SCHEMA_NAME SCH, RDB$TABLE_NAME T FROM RDB$PUBLICATION_TABLES ORDER BY RDB$TABLE_NAME');
        console.log(`-- ${when}`);
        for (const p of pubs)
            console.log(`   publication ${s(p.P)}: active=${p.A} auto_enable=${p.AE}`);
        console.log(`   published tables: ${tabs.map(t => s(t.SCH) + '.' + s(t.T)).join(', ') || '(none)'}\n`);
    };

    // Idempotent reset: back to a clean, unpublished state.
    for (const sql of ['ALTER DATABASE EXCLUDE ALL FROM PUBLICATION',
                       'ALTER DATABASE DISABLE PUBLICATION',
                       'DROP TABLE REPL_ORDERS', 'DROP TABLE REPL_SCRATCH'])
        try { await conn.query(sql); } catch (e) {}
    await conn.query('CREATE TABLE REPL_ORDERS (ID INT NOT NULL PRIMARY KEY, ITEM VARCHAR(30))');
    await conn.query('CREATE TABLE REPL_SCRATCH (N INT)');   // note: no key

    await pubState('initial state (publication exists but is inactive)');

    await conn.query('ALTER DATABASE ENABLE PUBLICATION');
    await pubState('after ENABLE PUBLICATION');

    await conn.query('ALTER DATABASE INCLUDE TABLE REPL_ORDERS TO PUBLICATION');
    await pubState('after INCLUDE TABLE REPL_ORDERS');

    await conn.query('ALTER DATABASE INCLUDE ALL TO PUBLICATION');
    await pubState('after INCLUDE ALL (future tables join automatically)');

    const mon = await conn.query('SELECT MON$REPLICA_MODE M FROM MON$DATABASE');
    console.log(`MON$REPLICA_MODE = ${mon[0].M} (0 = not a replica: this is a publishing primary)`);

    await conn.detach();
    console.log('done.');
    process.exit(0);
})().catch(e => { console.error('FAILED:', e.message || e); process.exit(1); });
