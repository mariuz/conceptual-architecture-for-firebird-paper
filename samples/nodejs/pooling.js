//
// pooling.js — both directions of pooling from ../../connection-pooling.md.
//
//   1. OUTBOUND: the server's own external-connections (EDS) pool, driven
//      and observed purely through SQL — three EXECUTE STATEMENT ON EXTERNAL
//      calls reuse one outbound connection, watched via the
//      EXT_CONN_POOL_* context variables.
//   2. INBOUND: node-firebird's client-side pool (Firebird.pool) — the
//      driver-level pooling that stands in for the inbound pooler Firebird
//      deliberately does not ship.
//
'use strict';

const { Firebird, DEFAULTS, attach, s } = require('./common');

const POOL_VARS =
    "select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT')   as idle," +
    "       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT') as act" +
    "  from rdb$database";

async function outbound() {
    console.log('--- outbound: the server-side EDS pool ---');
    const c = await attach();                      // employee, read-only use

    await c.query('alter external connections pool set size 5');
    await c.query('alter external connections pool set lifetime 30 second');

    let r = (await c.query(POOL_VARS))[0];
    console.log(`before:           idle=${s(r.IDLE)} active=${s(r.ACT)}`);

    // Three external calls, one pooled outbound connection (same DSN+user).
    r = (await c.query(
        `execute block returns (idle varchar(10), act varchar(10)) as
           declare i int = 0;
           declare v int;
         begin
           while (i < 3) do
           begin
             execute statement 'select 1 from rdb$database'
               on external 'inet://localhost/employee'
               as user '${DEFAULTS.user}' password '${DEFAULTS.password}'
               into :v;
             i = i + 1;
           end
           idle = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');
           act  = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');
           suspend;
         end`))[0];
    console.log(`inside the block: idle=${s(r.IDLE)} active=${s(r.ACT)}   (3 calls, 1 connection)`);

    // db.query auto-commits, so by now the external connection is reset
    // (ALTER SESSION RESET) and parked on the idle list.
    r = (await c.query(POOL_VARS))[0];
    console.log(`after commit:     idle=${s(r.IDLE)} active=${s(r.ACT)}`);

    await c.query('alter external connections pool clear all');
    r = (await c.query(POOL_VARS))[0];
    console.log(`after CLEAR ALL:  idle=${s(r.IDLE)} active=${s(r.ACT)}`);
    await c.detach();
}

const get = pool => new Promise((res, rej) =>
    pool.get((err, db) => err ? rej(err) : res(db)));
const sleep = ms => new Promise(res => setTimeout(res, ms));

async function inbound() {
    console.log('--- inbound: node-firebird\'s client-side pool ---');
    const pool = Firebird.pool(2, { ...DEFAULTS });    // max 2 connections

    const a = await get(pool);
    const b = await get(pool);
    console.log(`took 2 of max 2:  total=${pool.totalCount} active=${pool.activeCount} waiting=${pool.waitingCount}`);

    const t0 = Date.now();
    const third = get(pool);                 // no free slot: this queues
    await sleep(200);
    console.log(`asked for a 3rd:  total=${pool.totalCount} active=${pool.activeCount} waiting=${pool.waitingCount}`);

    a.detach();                              // pooled: returns the slot, no op_detach
    const c = await third;
    console.log(`released one:     3rd get served after ${Date.now() - t0} ms`);

    const row = (await new Promise((res, rej) =>
        c.query('select current_connection from rdb$database',
            (e, rows) => e ? rej(e) : res(rows))))[0];
    console.log(`pooled attachment works: CURRENT_CONNECTION = ${row.CURRENT_CONNECTION}`);

    b.detach();
    c.detach();
    await new Promise(res => pool.destroy(res));   // real op_detach for all
    console.log('pool destroyed. done.');
}

outbound()
    .then(inbound)
    .catch(err => { console.error('ERR:', err.message); process.exit(1); });
