//
// careful_writes.js — companion sample for ../../careful-writes-and-crash-safety.md.
//
// node-firebird speaks the wire protocol to a remote server, so it cannot
// crash the engine the way ../cpp/careful_writes.cpp does (there the child
// process IS the embedded engine).  What a client *can* demonstrate is the
// reader-side half of the same guarantee: a writer process SIGKILLed with an
// open transaction loses exactly its uncommitted work — the server treats
// the severed connection as an abrupt rollback, the same MGA visibility rule
// that makes a real crash recoverable with no log replay.
//
'use strict';

const { spawn } = require('child_process');
const { attachOrCreate, attach, ISOLATION } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/careful_writes_js.fdb';

// Writer mode: commit a marker, then insert uncommitted rows forever.
async function writer() {
    const conn = await attachOrCreate({ database: DB });
    try { await conn.query('drop table cw'); } catch (e) {}
    await conn.query('create table cw (id int, tag varchar(30))');
    await conn.query("insert into cw values (1, 'committed-marker')");
    console.log('WRITER: marker committed');            // parent watches for this

    const tx = await conn.transaction(ISOLATION.SNAPSHOT);   // never committed
    for (let i = 0; ; i++) {
        await tx.query("insert into cw values (?, 'uncommitted')", [i + 1000]);
        if (i > 0 && i % 2000 === 0)
            console.log(`WRITER: ${i} uncommitted rows`);     // parent kills us here
    }
}

// Parent mode: spawn the writer, kill it mid-transaction, verify.
async function parent() {
    const child = spawn(process.execPath, [__filename, '--writer'],
        { stdio: ['ignore', 'pipe', 'inherit'] });

    await new Promise(resolve => {
        child.stdout.on('data', chunk => {
            process.stdout.write(chunk);
            if (chunk.toString().includes('4000 uncommitted rows')) {
                console.log(`SIGKILL to writer pid ${child.pid} (mid-transaction)`);
                child.kill('SIGKILL');
                resolve();
            }
        });
    });
    await new Promise(r => child.on('exit', r));

    const t0 = Date.now();
    const conn = await attach({ database: DB });
    const [m] = await conn.query("select count(*) n from cw where tag = 'committed-marker'");
    const [u] = await conn.query("select count(*) n from cw where tag = 'uncommitted'");
    console.log(`re-attach + counts took ${Date.now() - t0} ms`);
    console.log(`committed marker rows : ${m.N}   <- durable`);
    console.log(`uncommitted rows      : ${u.N}   <- gone with the killed writer`);
    await conn.detach();
}

(process.argv[2] === '--writer' ? writer() : parent())
    .catch(e => { console.error('ERROR:', e.message || e); process.exit(1); });
