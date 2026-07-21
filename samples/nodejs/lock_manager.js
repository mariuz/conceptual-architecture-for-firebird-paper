//
// lock_manager.js — companion sample for ../../lock-manager.md.
//
// node-firebird cannot express RESERVING (its table-reservation TPB code is
// commented out as TODO), so this twin drives the same three lck_wait modes
// through ROW conflicts instead: waiting on a locked record means waiting on
// the blocking transaction's LCK_tra lock, so the timings below are the same
// enqueue / wait_for_request paths as the C++ sample — reached through MVCC.
// The driver's transaction options map straight onto the TPB:
//     { wait: false }               -> isc_tpb_nowait        (lck_wait == 0)
//     { wait: true, waitTimeout: 3 } -> isc_tpb_lock_timeout (lck_wait < 0)
//     { wait: true }                -> isc_tpb_wait          (lck_wait > 0)
//
'use strict';

const { attachOrCreate, attach } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/lock_manager_js.fdb';
const RC = [15, 17];    // isc_tpb_read_committed + rec_version
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function probe(conn, label, options) {
    const t0 = Date.now();
    const tx = await conn.transaction(options);
    try {
        await tx.query('update t1 set v = v + 1 where id = 1');
        console.log(`${label.padEnd(16)} granted after ${(Date.now() - t0) / 1000} s`);
        await tx.commit();
    } catch (e) {
        console.log(`${label.padEnd(16)} failed after ${(Date.now() - t0) / 1000} s: `
            + (e.message || e).split('\n')[0]);
        await tx.rollback();
    }
}

(async () => {
    const holder = await attachOrCreate({ database: DB });
    const prober = await attach({ database: DB });

    try { await holder.query('drop table t1'); } catch (e) {}
    await holder.query('create table t1 (id int primary key, v int)');
    await holder.query('insert into t1 values (1, 0)');

    // The holder's uncommitted update makes row 1 contested: every probe
    // below ends up waiting on the holder transaction's LCK_tra lock.
    const hold = await holder.transaction({ isolation: RC });
    await hold.query('update t1 set v = 100 where id = 1');
    console.log('holder: row 1 updated, uncommitted (LCK_tra held)');

    await probe(prober, 'NO WAIT:', { isolation: RC, wait: false });
    await probe(prober, 'LOCK TIMEOUT 3:', { isolation: RC, wait: true, waitTimeout: 3 });

    // WAIT parks until the holder commits, 2 s from now; READ COMMITTED
    // rec_version then proceeds against the newest version and succeeds.
    setTimeout(async () => {
        await hold.commit();
        console.log('holder: committed (2 s later) -> lock released');
    }, 2000);
    await probe(prober, 'WAIT:', { isolation: RC, wait: true });

    await prober.detach();
    await holder.detach();
})().catch(e => { console.error('ERROR:', e.message || e); process.exit(1); });
