//
// parallel-workers.js — the knobs, the poll, and an honest zero.
//
// Companion to ../../parallel-workers.md and JS twin of
// ../cpp/parallel_workers.cpp.  Both ParallelWorkers and MaxParallelWorkers
// are GLOBAL config, readable by any client through RDB$CONFIG — and on the
// live server both sit at their default of 1, so no CREATE INDEX from any
// client can spawn a worker here.  This sample demonstrates the observation
// technique anyway: build a table, CREATE INDEX from one connection while a
// second connection polls MON$ATTACHMENTS for MON$USER = '<Worker>', and
// report the zero for what it is: the ceiling working as configured.  (The
// C++ twin gets its non-zero the only way possible without touching the
// server: an embedded engine with a private firebird.conf.)
//
'use strict';

const { attachOrCreate, attach, s } = require('./common');

const DB = '/tmp/fbhandson/parallel.fdb';

(async () => {
    const conn = await attachOrCreate({ database: DB });

    // 1. The knobs, SQL-visible to every client.
    const cfg = await conn.query(
        "select rdb$config_name as k, rdb$config_value as v from rdb$config " +
        "where rdb$config_name in ('ParallelWorkers', 'MaxParallelWorkers')");
    for (const r of cfg) console.log(`${s(r.K).padEnd(19)} = ${s(r.V)}`);

    // ...including what THIS attachment was granted (dpb / config default):
    const mine = await conn.query(
        'select mon$parallel_workers as pw from mon$attachments ' +
        'where mon$attachment_id = current_connection');
    console.log(`this attachment's MON$PARALLEL_WORKERS = ${mine[0].PW}`);

    // 2. Something parallelizable: a table wide enough to span pointer pages.
    try { await conn.query('drop table parade'); } catch (e) {}
    await conn.query('create table parade (id int, val varchar(200))');
    await conn.query(
        'execute block as declare n int = 0; begin ' +
        '  while (n < 50000) do begin ' +
        '    insert into parade values (:n, ' +
        '      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || ' +
        '      uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || ' +
        '      uuid_to_char(gen_uuid())); n = n + 1; end end');

    // 3. CREATE INDEX on connection 1; poll MON$ATTACHMENTS on connection 2.
    const monitor = await attach({ database: DB });
    let maxWorkers = 0, polls = 0;
    const poller = setInterval(async () => {
        try {
            const rows = await monitor.query(
                "select count(*) as n from mon$attachments " +
                "where mon$user = '<Worker>'");
            polls++;
            maxWorkers = Math.max(maxWorkers, rows[0].N);
        } catch (e) { /* mon$ poll raced the detach */ }
    }, 50);

    const t0 = Date.now();
    await conn.query('create index ix_parade on parade (val)');
    const ms = Date.now() - t0;
    clearInterval(poller);

    console.log(`create index: ${ms} ms; ` +
        `'<Worker>' attachments seen in ${polls} polls: ${maxWorkers}`);
    console.log(maxWorkers === 0
        ? 'zero, as configured: MaxParallelWorkers = 1 means the pool can never' +
          '\ngrow, whatever any attachment requests — the honest result here.'
        : 'workers observed - this server has MaxParallelWorkers > 1.');

    await monitor.detach();
    await conn.detach();
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
