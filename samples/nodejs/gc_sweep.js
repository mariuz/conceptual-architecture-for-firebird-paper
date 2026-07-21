//
// gc_sweep.js — companion sample for ../../garbage-collection-and-sweep.md.
//
// Same experiment as ../cpp/gc_sweep.cpp through the pure-JS wire protocol
// driver: create record versions under a pinned SNAPSHOT, release it, and
// watch the collectors through MON$RECORD_STATS (MON$RECORD_IMGC = FB5's
// intermediate GC, MON$RECORD_PURGES / MON$RECORD_EXPUNGES = vio.cpp's
// purge() and expunge()), plus the OIT/OAT/OST/Next header counters.
//
'use strict';

const { attachOrCreate, attach, ISOLATION, s } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/gc_sweep.fdb';
const sleep = ms => new Promise(r => setTimeout(r, ms));

// db.query() runs each call in its own transaction, so every peek at the
// MON$ tables gets a fresh monitoring snapshot.
async function stats(conn, label) {
    const [r] = await conn.query(
        `select r.MON$RECORD_UPDATES upd, r.MON$RECORD_IMGC imgc,
                r.MON$RECORD_PURGES purges, r.MON$RECORD_EXPUNGES expunges
         from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)`);
    console.log(`${label.padEnd(34)} upd=${r.UPD} imgc=${r.IMGC}`
        + ` purges=${r.PURGES} expunges=${r.EXPUNGES}`);
}

async function counters(conn, label) {
    const [c] = await conn.query(
        `select MON$OLDEST_TRANSACTION oit, MON$OLDEST_ACTIVE oat,
                MON$OLDEST_SNAPSHOT ost, MON$NEXT_TRANSACTION nxt
         from MON$DATABASE`);
    console.log(`${label.padEnd(34)} OIT=${c.OIT} OAT=${c.OAT} OST=${c.OST} Next=${c.NXT}`);
}

(async () => {
    const writer = await attachOrCreate({ database: DB });
    const pinner = await attach({ database: DB });

    try { await writer.query('drop table gctest'); } catch (e) {}
    await writer.query('create table gctest (id int primary key, val int)');
    await writer.query('insert into gctest values (1, 0)');

    // 1. Pin a snapshot: its tra_oldest_active holds the OST down.
    const snap = await pinner.transaction(ISOLATION.SNAPSHOT);
    const [v0] = await snap.query('select val from gctest where id = 1');
    console.log(`pinned SNAPSHOT reads val = ${v0.VAL}`);
    await stats(writer, 'before updates:');

    // 2. Twelve committed updates -> twelve back versions... in theory.
    for (let i = 1; i <= 12; i++) {
        const t = await writer.transaction(ISOLATION.SNAPSHOT);
        await t.query('update gctest set val = ? where id = 1', [i]);
        await t.commit();
    }
    await stats(writer, 'after 12 updates (snapshot open):');
    const [v1] = await snap.query('select val from gctest where id = 1');
    console.log(`pinned SNAPSHOT still reads val = ${v1.VAL}`);

    // 3. Release the snapshot; the next scan feeds the below-OST chain to
    //    cooperative GC and/or the background GC thread.
    await snap.commit();
    const [v2] = await writer.query('select val from gctest where id = 1');
    console.log(`snapshot released; new reader sees val = ${v2.VAL}`);
    await sleep(1500);
    await stats(writer, 'after release + scan + 1.5s:');

    // 4. A committed DELETE older than the OST is expunged, not purged.
    await writer.query('delete from gctest where id = 1');
    await writer.query('select count(*) n from gctest');
    await sleep(1500);
    await stats(writer, 'after DELETE + scan + 1.5s:');

    // 5. A rolled-back stump pins the OIT.  TPB [2, 20] = isc_tpb_concurrency
    //    + isc_tpb_no_auto_undo: no in-memory undo log, so rollback is
    //    recorded in the TIP and stays "interesting" until sweep.
    await counters(writer, 'header counters before rollback:');
    const stump = await writer.transaction([2, 20]);
    await stump.query('insert into gctest values (2, 0)');
    await stump.rollback();
    await counters(writer, 'after no_auto_undo rollback:');
    console.log("run 'gfix -sweep' to move the OIT past the stump.");

    await pinner.detach();
    await writer.detach();
})().catch(e => { console.error('ERROR:', s(e.message || e)); process.exit(1); });
