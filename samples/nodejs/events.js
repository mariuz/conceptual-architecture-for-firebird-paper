//
// events.js — POST_EVENT notifications through node-firebird's pure-JS
// wire-protocol driver.  Companion to ../../firebird-events.md.
//
// The driver implements the full auxiliary-channel dance of that document:
// db.attachEvent() sends op_connect_request and opens the second TCP
// connection (lib/wire/eventConnection.js), registerEvent() sends
// op_que_events, and each op_event arriving on the aux socket is emitted as
// 'post_event' (name, count) — after which the driver re-queues, because
// interests are one-shot.  Two attachments: a listener and a poster.
//
'use strict';

const { attach } = require('./common');

const sleep = ms => new Promise(res => setTimeout(res, ms));
const EVENT = 'demo_event';
const POST = "execute block as begin post_event 'demo_event'; end";

async function main() {
    // --- listener: aux connection + one-shot interest, auto re-queued ------
    const listener = await attach();
    const evtmgr = await new Promise((res, rej) =>
        listener.db.attachEvent((err, mgr) => err ? rej(err) : res(mgr)));

    // NOTE: unlike the C++ isc_event_counts idiom (which subtracts the
    // baseline and yields "fired N times since you last looked"),
    // node-firebird emits the raw RUNNING COUNTER of the shared-memory evnt
    // block — the delta is ours to compute against the baseline delivery.
    const delivered = [];                       // every op_event we receive
    let baseline = 0;
    evtmgr.on('post_event', (name, count) => delivered.push({ name, count }));

    await new Promise((res, rej) =>
        evtmgr.registerEvent([EVENT], err => err ? rej(err) : res()));
    await sleep(300);                           // let the baseline (if any) land
    if (delivered.length)                       // first delivery = current count
        baseline = delivered[0].count;
    delivered.length = 0;
    console.log(`listener subscribed to '${EVENT}' over the aux connection (baseline counter = ${baseline})`);

    // --- poster: rollback swallows posts -----------------------------------
    const poster = await attach();
    let tx = await poster.transaction();
    await tx.query(POST);
    await tx.rollback();
    await sleep(500);
    console.log(`after POST_EVENT + ROLLBACK : ${delivered.length} deliveries (rollback swallows posts)`);

    // --- delivery is commit-time, posts coalesce ---------------------------
    tx = await poster.transaction();
    await tx.query(POST);
    await tx.query(POST);
    await tx.query(POST);
    await sleep(500);
    console.log(`3 posts, before COMMIT      : ${delivered.length} deliveries (delivery is commit-time)`);

    await tx.commit();
    await sleep(500);
    for (const d of delivered)
        console.log(`after COMMIT                : '${d.name}' counter=${d.count}, ` +
            `delta=${d.count - baseline} (one delivery, 3 posts coalesced)`);

    // --- cleanup ------------------------------------------------------------
    await new Promise(res => evtmgr.close(() => res()));
    await poster.detach();
    await listener.detach();
    console.log('done.');
}

main().catch(err => { console.error('ERR:', err.message); process.exit(1); });
