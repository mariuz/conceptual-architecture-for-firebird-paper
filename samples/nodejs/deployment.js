//
// deployment.js — companion sample for ../../deployment-and-operations.md
//
// The server's own view of its deployment, read over the wire protocol:
// MON$DATABASE (physical facts), RDB$CONFIG (effective configuration) and
// the SYSTEM context namespace.  Read-only; runs against employee.
//
'use strict';

const { attach, s } = require('./common');

(async () => {
    const conn = await attach();   // employee, charset NONE

    console.log('== MON$DATABASE: the database as deployed ==');
    const mon = (await conn.query(
        `SELECT MON$DATABASE_NAME DB, MON$ODS_MAJOR ODS_MAJ, MON$ODS_MINOR ODS_MIN,
                MON$PAGE_SIZE PG, MON$PAGE_BUFFERS BUF, MON$SWEEP_INTERVAL SWEEP,
                MON$FORCED_WRITES FW, MON$SQL_DIALECT DIA, MON$CRYPT_STATE CRYPT
         FROM MON$DATABASE`))[0];
    console.log(`  database file:  ${s(mon.DB)}`);
    console.log(`  ODS ${mon.ODS_MAJ}.${mon.ODS_MIN}, page size ${mon.PG}, page buffers ${mon.BUF}`);
    console.log(`  sweep interval ${mon.SWEEP}, forced writes ${mon.FW}, dialect ${mon.DIA}, crypt state ${mon.CRYPT}`);

    console.log('\n== RDB$CONFIG: effective configuration ==');
    const cfg = await conn.query(
        `SELECT RDB$CONFIG_NAME N, RDB$CONFIG_VALUE V, RDB$CONFIG_IS_SET SET_
         FROM RDB$CONFIG
         WHERE RDB$CONFIG_NAME IN ('ServerMode', 'DefaultDbCachePages',
           'DatabaseAccess', 'WireCrypt', 'MaxParallelWorkers', 'SecurityDatabase')
         ORDER BY RDB$CONFIG_NAME`);
    for (const r of cfg)
        console.log(`  ${s(r.N).padEnd(20)} = ${s(r.V).padEnd(28)} (explicitly set: ${r.SET_})`);
    const nset = await conn.query(
        'SELECT COUNT(*) N FROM RDB$CONFIG WHERE RDB$CONFIG_IS_SET');
    console.log(`  settings explicitly set in config files: ${nset[0].N}`);

    console.log('\n== SYSTEM context: this engine, this session ==');
    for (const v of ['ENGINE_VERSION', 'DB_NAME', 'NETWORK_PROTOCOL',
                     'WIRE_CRYPT_PLUGIN', 'CLIENT_ADDRESS']) {
        const r = await conn.query(
            `SELECT RDB$GET_CONTEXT('SYSTEM', ?) V FROM RDB$DATABASE`, [v]);
        console.log(`  ${v.padEnd(18)} ${s(r[0].V)}`);
    }

    await conn.detach();
    console.log('\ndone.');
    process.exit(0);
})().catch(e => { console.error('FAILED:', e.message || e); process.exit(1); });
