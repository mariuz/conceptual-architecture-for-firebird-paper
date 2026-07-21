//
// sorting.js — companion to ../../sorting-and-temp-space.md
//
// The MON$ view of the TempCacheLimit threshold.  One connection runs the
// same two ORDER BY queries as ../cpp/sorting.cpp (both PLAN SORT ...
// NATURAL); a second connection polls database-level MON$MEMORY_USAGE in a
// loop — every db.query runs in its own transaction, and MON$ snapshots are
// per-transaction, so each poll is fresh.  The big sort drives allocated
// memory up by ~TempCacheLimit (64 MB) and then spills the rest to an
// unlinked fb_sort_* scratch file (visible only in /proc — see the C++
// sample for that half); the small sort stays inside the budget.
//
// Reuses the bulk table the C++ sample builds; creates it if missing.
// Run: node sorting.js
//
'use strict';

const { attachOrCreate, attach } = require('./common');

const DB = '/tmp/fbhandson/sorting.fdb';
const MEMSQL = 'select m.mon$memory_allocated a from mon$database d ' +
    'join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id';

async function ensureBulk(conn) {
    try {
        const [r] = await conn.query('select count(*) c from bulk');
        if (r.C === 200000) return;
    } catch (e) { /* table missing */ }
    console.log('building bulk (200000 rows)...');
    await conn.query('recreate table bulk (id integer, pad varchar(400) character set ascii)');
    await conn.query(
        `execute block as declare i integer = 0; begin
           while (i < 200000) do begin
             insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x'));
             i = i + 1;
           end
         end`);
}

// Run fn() while polling MON$ memory from a second connection; return peak.
async function watchMemory(mon, fn) {
    let peak = 0, stop = false;
    const poller = (async () => {
        while (!stop) {
            const [r] = await mon.query(MEMSQL);
            if (r.A > peak) peak = r.A;
        }
    })();
    const result = await fn();
    stop = true;
    await poller;
    return { peak, result };
}

(async () => {
    const conn = await attachOrCreate({ database: DB });
    const mon = await attach({ database: DB });
    await ensureBulk(conn);

    const [idle] = await mon.query(MEMSQL);
    console.log(`database memory allocated while idle: ${idle.A} bytes`);

    const cases = [
        ['big sort (200k rows, ~82 MB of sort data)',
            'select first 1 id from bulk order by pad desc'],
        ['small sort (20k rows, ~8 MB)',
            'select first 1 id from bulk where mod(id, 10) = 0 order by pad desc'],
    ];
    for (const [label, sql] of cases) {
        const { peak, result } = await watchMemory(mon, () => conn.query(sql));
        console.log(`\n${label}`);
        console.log(`  top row id = ${result[0].ID}`);
        console.log(`  peak database MON$MEMORY_ALLOCATED: ${peak} bytes` +
            ` (+${peak - idle.A} over idle)`);
    }

    await mon.detach();
    await conn.detach();
    console.log('\ndone.');
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
