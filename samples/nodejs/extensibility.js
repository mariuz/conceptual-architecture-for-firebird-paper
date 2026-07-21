//
// extensibility.js — a pure-JS client installing and calling native code.
//
// Companion to ../../extensibility.md and JS twin of ../cpp/extensibility.cpp.
// The interesting asymmetry with the embedded/architecture samples: here the
// pure-JS driver loses NOTHING, because the extension seam is on the SERVER
// side.  EXTERNAL NAME ... ENGINE udr is plain DDL and gen_rows(1,5) is a
// plain SELECT — the udr_engine plugin loads libudrcpp_example.so inside the
// server process, and the wire protocol neither knows nor cares that the
// rows were produced by compiled C++.
//
'use strict';

const { attachOrCreate, s } = require('./common');

(async () => {
    const conn = await attachOrCreate({ database: '/tmp/fbhandson/extensibility.fdb' });

    // 1. Bind SQL names to entry points in the shipped native UDR module.
    await conn.query(
        "recreate procedure gen_rows (start_n integer not null, " +
        "                             end_n integer not null) " +
        "  returns (n integer not null) " +
        "  external name 'udrcpp_example!gen_rows' engine udr");
    await conn.query(
        "recreate function sum_args (n1 integer, n2 integer, n3 integer) " +
        "  returns integer " +
        "  external name 'udrcpp_example!sum_args' engine udr");

    // 2. Call them.
    const rows = await conn.query('select n from gen_rows(1, 5)');
    console.log('gen_rows(1, 5)   ->', rows.map(r => r.N).join(', '),
        '   (C++ running in the server)');

    const sum = await conn.query(
        'select sum_args(19, 20, 3) as total from rdb$database');
    console.log('sum_args(19,20,3) ->', sum[0].TOTAL);

    // 3. The plugin roster, visible to any client that can SELECT.
    const cfg = await conn.query(
        "select rdb$config_name as role, rdb$config_value as plugin " +
        "from rdb$config " +
        "where rdb$config_name in ('Providers', 'AuthServer', 'UserManager', " +
        "      'WireCryptPlugin', 'TracePlugin', 'DefaultProfilerPlugin') " +
        "order by rdb$config_id");
    console.log('\nplugins filling each role (rdb$config):');
    for (const r of cfg)
        console.log(`    ${s(r.ROLE).padEnd(22)} ${s(r.PLUGIN)}`);

    await conn.detach();
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
