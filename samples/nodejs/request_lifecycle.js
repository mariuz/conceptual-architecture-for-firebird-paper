//
// request_lifecycle.js — companion to ../../request-lifecycle-code-trace.md
//
// The same instrumented CREATE TABLE round trip as ../cpp/request_lifecycle.cpp,
// through node-firebird — so Stage 2's PACKET/XDR layer is JavaScript here,
// not fbclient.  The driver has no separate prepare call, so the DDL runs in
// one op_execute round trip inside an explicit transaction; the MON$ counters
// still expose the engine stages, and a second transaction on the same
// connection shows the catalog write is invisible until TRA_commit flips it.
//
'use strict';

const { attachOrCreate, s } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/request_lifecycle.fdb';

async function sample(conn) {
    const r = (await conn.query(
        'SELECT i.MON$PAGE_MARKS AS MARKS, i.MON$PAGE_WRITES AS WRITES,'
        + ' r.MON$RECORD_INSERTS AS RECINS'
        + ' FROM MON$ATTACHMENTS a'
        + ' JOIN MON$IO_STATS i ON a.MON$STAT_ID = i.MON$STAT_ID'
        + ' JOIN MON$RECORD_STATS r ON a.MON$STAT_ID = r.MON$STAT_ID'
        + ' WHERE a.MON$ATTACHMENT_ID = CURRENT_CONNECTION'))[0];
    return { marks: Number(r.MARKS), writes: Number(r.WRITES), recIns: Number(r.RECINS) };
}

const count = async (q, label) => s((await q.query('SELECT COUNT(*) AS C'
    + " FROM RDB$RELATIONS WHERE RDB$RELATION_NAME = 'TRACE_DEMO'"))[0].C);

(async () => {
    const conn = await attachOrCreate({ database: DB, encoding: 'UTF8' });
    try {
        try { await conn.query('DROP TABLE trace_demo'); } catch (e) { /* first run */ }

        const s0 = await sample(conn);
        const tx = await conn.transaction();

        let t0 = Date.now();
        await tx.query('CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY,'
            + ' name VARCHAR(30))');
        const tExec = Date.now() - t0;
        const s1 = await sample(conn);
        console.log(`execute  ${tExec} ms   catalog record inserts: +${s1.recIns - s0.recIns},`
            + ` page marks: +${s1.marks - s0.marks}`);

        // The STORE into RDB$RELATIONS is transaction-private until commit:
        console.log('         in the DDL tx:      TRACE_DEMO rows in RDB$RELATIONS =',
            await count(tx));
        const other = await conn.transaction();
        console.log('         in another tx:      TRACE_DEMO rows in RDB$RELATIONS =',
            await count(other));
        await other.rollback();

        t0 = Date.now();
        await tx.commit();               // TRA_commit -> DFW -> CCH_flush -> PIO_write
        const tCommit = Date.now() - t0;
        const s2 = await sample(conn);
        console.log(`commit   ${tCommit} ms   page writes: +${s2.writes - s1.writes}`);

        console.log('         after commit:       TRACE_DEMO rows in RDB$RELATIONS =',
            await count(conn));
    } finally {
        await conn.detach();
    }
})().catch(e => { console.error(e.message || e); process.exit(1); });
