//
// monitoring.js — companion sample for ../../monitoring-and-tuning.md
//
// The MON$ hierarchy and the per-transaction snapshot property, through
// node-firebird.  Explicit transactions matter here: each driver-level
// query outside one runs in its own transaction and would therefore get a
// fresh MON$ snapshot every time — the C++ sample's "stale counters" effect
// only reproduces inside one explicit transaction.
//
'use strict';

const { attachOrCreate, ISOLATION, s } = require('./common');

const DB = '/tmp/fbhandson/monitoring_js.fdb';

const COUNTERS = `
    SELECT R.MON$RECORD_SEQ_READS SEQ, R.MON$RECORD_IDX_READS IDX,
           R.MON$RECORD_INSERTS INS, I.MON$PAGE_FETCHES FETCHES
    FROM MON$ATTACHMENTS A
    JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID
    JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID
    WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION`;

const show = (label, c) =>
    console.log(`${label.padEnd(38)} seq=${c.SEQ} idx=${c.IDX} ins=${c.INS} fetches=${c.FETCHES}`);

(async () => {
    const conn = await attachOrCreate({ database: DB, encoding: 'UTF8' });

    try { await conn.query('DROP TABLE MON_WORK'); } catch (e) {}
    await conn.query('CREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)');
    await conn.query(
        `EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN
           WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); I = I + 1; END
         END`);

    // The hierarchy: database -> my attachment -> transaction -> statement.
    const tx = await conn.transaction(ISOLATION.SNAPSHOT);
    const mdb = (await tx.query(
        `SELECT MON$OLDEST_TRANSACTION OIT, MON$OLDEST_ACTIVE OAT, MON$NEXT_TRANSACTION NXT
         FROM MON$DATABASE`))[0];
    console.log(`MON$DATABASE: OIT=${mdb.OIT} OAT=${mdb.OAT} NEXT=${mdb.NXT}`);
    const me = (await tx.query(
        `SELECT A.MON$ATTACHMENT_ID ATT, A.MON$REMOTE_PROTOCOL PROTO, T.MON$TRANSACTION_ID TRA,
                CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 30) AS VARCHAR(30)) SQL_HEAD
         FROM MON$ATTACHMENTS A
         JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID
         JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID
         WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION`))[0];
    console.log(`me: attachment ${me.ATT} (${s(me.PROTO)}), tx ${me.TRA}, running: ${s(me.SQL_HEAD)}...\n`);

    // The snapshot property, on this attachment's own counters.
    show('counters (snapshot 1):', (await tx.query(COUNTERS))[0]);

    const n = (await tx.query('SELECT COUNT(*) N FROM MON_WORK'))[0].N;
    console.log(`... workload: full scan counted ${n} rows ...`);

    show('same tx, re-queried (STILL snapshot 1):', (await tx.query(COUNTERS))[0]);
    await tx.commit();

    const tx2 = await conn.transaction(ISOLATION.SNAPSHOT);
    show('new tx (fresh snapshot):', (await tx2.query(COUNTERS))[0]);
    await tx2.commit();

    await conn.detach();
    console.log('done.');
    process.exit(0);
})().catch(e => { console.error('FAILED:', e.message || e); process.exit(1); });
