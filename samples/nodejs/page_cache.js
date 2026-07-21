//
// page_cache.js — companion sample for ../../page-cache-coherency.md.
//
// node-firebird is a wire-protocol client: it can only reach the server's
// ONE shared page cache, so this is the twin of page_cache.cpp's phase 1.
// (Phase 2 — private caches kept coherent by LCK_bdb page locks — needs two
// embedded engine processes and exists only on the C++ side.)  Two
// connections ping-pong updates on two rows of the SAME data page; the
// per-attachment MON$IO_STATS then show what "shared cache" means: page
// fetches (logical) in the thousands, page reads (physical) near zero.
//
'use strict';

const { attachOrCreate, attach, ISOLATION } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/page_cache_js.fdb';
const ROUNDS = 300;

async function io(conn, label) {
    const [r] = await conn.query(
        `select MON$PAGE_FETCHES f, MON$PAGE_READS r, MON$PAGE_WRITES w
         from MON$IO_STATS join MON$ATTACHMENTS using (MON$STAT_ID)
         where MON$ATTACHMENT_ID = CURRENT_CONNECTION`);
    console.log(`  ${label}: page fetches=${r.F} reads=${r.R} writes=${r.W}`);
}

async function worker(conn, rowId) {
    for (let i = 0; i < ROUNDS; i++) {
        const t = await conn.transaction(ISOLATION.READ_COMMITTED);
        await t.query('update t set v = v + 1 where id = ?', [rowId]);
        await t.commit();
    }
}

(async () => {
    const init = await attachOrCreate({ database: DB });
    try { await init.query('drop table t'); } catch (e) {}
    await init.query('create table t (id int primary key, v int)');
    await init.query('insert into t values (1, 0)');
    await init.query('insert into t values (2, 0)');
    await init.detach();

    const a = await attach({ database: DB });
    const b = await attach({ database: DB });

    console.log(`two connections, one shared cache: ${ROUNDS} commits each on one page`);
    await Promise.all([worker(a, 1), worker(b, 2)]);
    await io(a, 'conn A (row 1)');
    await io(b, 'conn B (row 2)');

    // Coherency across attachments is immediate — same buffers, no protocol.
    const [r1] = await a.query('select v from t where id = 2');
    const [r2] = await b.query('select v from t where id = 1');
    console.log(`  A sees B's row: v=${r1.V}; B sees A's row: v=${r2.V} (expected ${ROUNDS})`);

    await a.detach();
    await b.detach();
})().catch(e => { console.error('ERROR:', e.message || e); process.exit(1); });
