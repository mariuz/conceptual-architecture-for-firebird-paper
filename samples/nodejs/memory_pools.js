//
// memory_pools.js — companion sample for ../../memory-management.md.
//
// The pool hierarchy seen through MON$MEMORY_USAGE from node-firebird:
// the six stat groups with the parent-redirection signature (used > 0,
// allocated = 0 for child pools), one connection's own pool chain, and a
// transaction pool growing while an uncommitted UPDATE builds its undo log
// — watched from a second connection, since MON$ snapshots are frozen per
// transaction.
//
'use strict';

const { attachOrCreate, attach, ISOLATION } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/memory_pools_js.fdb';

async function poolRow(mon, label, join) {
    const rows = await mon.query(
        `select MON$MEMORY_USED u, MON$MEMORY_ALLOCATED a from MON$MEMORY_USAGE ${join}`);
    if (rows.length)
        console.log(`  ${label.padEnd(24)} used=${String(rows[0].U).padEnd(10)} allocated=${rows[0].A}`);
    return rows.length ? rows[0].U : 0;
}

(async () => {
    const worker = await attachOrCreate({ database: DB });
    const mon = await attach({ database: DB });

    try { await worker.query('drop table t'); } catch (e) {}
    await worker.query('create table t (id int, pad varchar(200))');
    await worker.query(
        `execute block as declare i int = 0; begin
           while (i < 3000) do begin
             insert into t values (:i, rpad('x', 200, 'x')); i = i + 1;
           end
         end`);

    console.log('-- per-level summary (0=db 1=att 2=tra 3=stmt 5=cmp_stmt)');
    const sum = await mon.query(
        `select MON$STAT_GROUP g, count(*) pools, sum(MON$MEMORY_USED) used,
                sum(MON$MEMORY_ALLOCATED) alloc
         from MON$MEMORY_USAGE group by 1 order by 1`);
    for (const r of sum)
        console.log(`  group ${r.G}: ${r.POOLS} pools  used=${String(r.USED).padEnd(10)} allocated=${r.ALLOC}`);

    const tx = await worker.transaction(ISOLATION.SNAPSHOT);
    const [ids] = await tx.query(
        'select current_connection att, current_transaction tra from rdb$database');

    console.log('\n-- worker pool chain (before)');
    await poolRow(mon, 'database pool:', 'join MON$DATABASE using (MON$STAT_ID)');
    await poolRow(mon, 'worker attachment pool:',
        `join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ${ids.ATT}`);
    await poolRow(mon, 'worker transaction pool:',
        `join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = ${ids.TRA}`);

    await tx.query("update t set pad = rpad('y', 200, 'y')");
    console.log('\n-- after an uncommitted 3000-row UPDATE');
    const attDuring = await poolRow(mon, 'worker attachment pool:',
        `join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ${ids.ATT}`);
    const traUsed = await poolRow(mon, 'worker transaction pool:',
        `join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = ${ids.TRA}`);

    await tx.rollback();
    console.log('\n-- after rollback (whole transaction pool freed at once)');
    const attAfter = await poolRow(mon, 'worker attachment pool:',
        `join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = ${ids.ATT}`);
    console.log(`  attachment used fell by ${attDuring - attAfter}; `
        + `the dead transaction pool held ${traUsed} — the nested roll-up, live`);

    await mon.detach();
    await worker.detach();
})().catch(e => { console.error('ERROR:', e.message || e); process.exit(1); });
