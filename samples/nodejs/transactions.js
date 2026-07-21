//
// transactions.js — MVCC isolation seen from the client (JavaScript twin of
// ../cpp/transactions_demo.cpp; see ../../transactions-and-concurrency.md).
//
// Two connections play the same scenario: SNAPSHOT stability, READ COMMITTED
// freshness, then a write conflict surfaced as SQLSTATE 40001.
//
// Run:  node transactions.js
//
'use strict';

const { attachOrCreate, attach, ISOLATION, s } = require('./common');

const DB = { database: process.argv[2] || '/tmp/fbhandson/tx.fdb' };

(async () => {
    const a = await attachOrCreate(DB);   // connection A
    const b = await attach(DB);           // connection B

    await a.query('recreate table balance (id integer primary key, amount integer)');
    await a.query('insert into balance values (1, 100)');

    // --- 1. SNAPSHOT stability ---------------------------------------------
    const snapA = await a.transaction(ISOLATION.SNAPSHOT);
    let r = await snapA.query('select amount from balance where id = 1');
    console.log('A (SNAPSHOT)       sees amount =', r[0].AMOUNT);

    await b.query('update balance set amount = 999 where id = 1');
    console.log('B                  committed amount = 999');

    r = await snapA.query('select amount from balance where id = 1');
    console.log('A (same SNAPSHOT)  sees amount =', r[0].AMOUNT,
        '  <- still the start-of-tx version');

    // --- 2. READ COMMITTED sees the new version ----------------------------
    const rcA = await a.transaction(ISOLATION.READ_COMMITTED);
    r = await rcA.query('select amount from balance where id = 1');
    console.log('A (READ COMMITTED) sees amount =', r[0].AMOUNT,
        '  <- the committed version');
    await rcA.commit();

    // --- 3. Write conflict -------------------------------------------------
    // B updates the row inside an open SNAPSHOT transaction; A, also under
    // SNAPSHOT, updates the same row and commits first.  When B's turn comes
    // the engine reports the update conflict (node-firebird's default TPB is
    // WAIT, so the conflict surfaces when the blocker commits).
    const holdB = await b.transaction(ISOLATION.SNAPSHOT);
    await holdB.query('update balance set amount = amount + 1 where id = 1');

    const loserA = await a.transaction(ISOLATION.SNAPSHOT);
    const race = loserA.query('update balance set amount = amount + 10 where id = 1')
        .then(() => 'unexpected: conflicting update succeeded')
        .catch(e => 'A conflicting update failed as designed:\n    ' + e.message);
    await new Promise(res => setTimeout(res, 300));   // let A block on the row
    await holdB.commit();                             // blocker commits -> conflict fires
    console.log(await race);

    await loserA.rollback();
    await snapA.commit();
    await a.detach();
    await b.detach();
    console.log('done.');
})().catch(e => { console.error(e); process.exit(1); });
