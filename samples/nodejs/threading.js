//
// threading.js — companion sample for ../../threading-and-synchronization.md.
//
// Same measurement as ../cpp/threading.cpp: node runs on the same host as
// the server, so it can pair MON$SERVER_PID with /proc/<pid>/task to count
// the engine's threads while opening attachments in parallel — SuperServer's
// thread-per-attachment topology and its thread pooling, watched from
// JavaScript.  The twist: here the twelve "client threads" are one node
// event loop multiplexing twelve sockets; the threads are all server-side.
//
'use strict';

const fs = require('fs');
const { attachOrCreate, attach, s } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/threading_js.fdb';
const sleep = ms => new Promise(r => setTimeout(r, ms));

const countThreads = pid => fs.readdirSync(`/proc/${pid}/task`).length;

(async () => {
    const db = await attachOrCreate({ database: DB });
    const [{ PID: pid }] = await db.query(
        `select MON$SERVER_PID pid from MON$ATTACHMENTS
         where MON$ATTACHMENT_ID = current_connection`);
    console.log(`engine process: pid ${pid}, ${countThreads(pid)} threads (1 attachment open)`);

    // Twelve concurrent attachments; node needs no threads of its own for
    // this — the concurrency lives entirely in the server process.
    const conns = await Promise.all(
        Array.from({ length: 12 }, () => attach({ database: DB })));
    await Promise.all(conns.map(c => c.query('select 1 from rdb$database')));

    const [{ N: users }] = await db.query(
        'select count(*) n from MON$ATTACHMENTS where MON$SYSTEM_FLAG = 0');
    const [{ N: pids }] = await db.query(
        'select count(distinct MON$SERVER_PID) n from MON$ATTACHMENTS');
    console.log(`with 12 extra attachments: ${countThreads(pid)} threads | `
        + `${users} user attachments, ${pids} distinct server pid`);

    await Promise.all(conns.map(c => c.detach()));
    await sleep(1000);
    console.log(`after they detach:        ${countThreads(pid)} threads (pooled)`);

    const sys = await db.query(
        `select MON$ATTACHMENT_ID id, MON$SYSTEM_FLAG sys, MON$USER usr,
                coalesce(MON$REMOTE_PROCESS, '<internal>') proc
         from MON$ATTACHMENTS order by MON$ATTACHMENT_ID`);
    for (const r of sys)
        console.log(`  att ${r.ID}  sys=${r.SYS}  ${s(r.USR).padEnd(18)} ${s(r.PROC)}`);
    await db.detach();
})().catch(e => { console.error('ERROR:', e.message || e); process.exit(1); });
