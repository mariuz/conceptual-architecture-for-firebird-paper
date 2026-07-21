//
// profiler.js — the accumulation view from a pure-JS client.
//
// Companion to ../../profiler.md and JS twin of ../cpp/profiler.cpp.  Like
// the extensibility sample, nothing is lost from JavaScript: the profiler's
// control surface is a SQL package (RDB$PROFILER) and its output is a SQL
// schema (PLG$PROFILER), so any client that can run queries can profile —
// the payoff of the document's "why the output is a schema" argument.
//
'use strict';

const { attachOrCreate, s } = require('./common');

(async () => {
    const conn = await attachOrCreate({ database: '/tmp/fbhandson/profiler.fdb' });

    // Fixtures (idempotent; shared with the C++ twin).
    try { await conn.query('drop procedure hotspot_js'); } catch (e) {}
    try { await conn.query('drop table nums_js'); } catch (e) {}
    await conn.query('create table nums_js (id int primary key, val int)');
    await conn.query(
        'execute block as declare n int = 0; begin ' +
        '  while (n < 5000) do begin ' +
        '    insert into nums_js values (:n, mod(:n, 97)); n = n + 1; end end');
    await conn.query(
        'create procedure hotspot_js returns (total bigint) as\n' +
        '  declare i int = 0;\n' +
        '  declare x int;\n' +
        'begin\n' +
        '  total = 0;\n' +
        '  while (i < 20000) do\n' +
        '  begin\n' +
        '    select val from nums_js where id = mod(:i, 5000) into :x;\n' +
        '    total = total + coalesce(:x, 0);\n' +
        '    i = i + 1;\n' +
        '  end\n' +
        '  suspend;\n' +
        'end');

    // Profile a workload.
    const [{ START_SESSION: profileId }] = await conn.query(
        "select rdb$profiler.start_session('js hands-on') from rdb$database");
    await conn.query('select total from hotspot_js');
    await conn.query('execute procedure rdb$profiler.finish_session(true)');
    console.log(`profile session ${profileId} finished and flushed\n`);

    // Read the per-line accumulation back — plain SQL, plain rows.
    const rows = await conn.query(
        'select line_num, column_num, counter, ' +
        '       total_elapsed_time as total_ns, avg_elapsed_time as avg_ns ' +
        'from plg$profiler.plg$prof_psql_stats_view ' +
        'where profile_id = ? and routine_name = ? ' +
        'order by total_elapsed_time desc', [profileId, 'HOTSPOT_JS']);

    console.log('hotspot_js, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):');
    console.log('line  col  counter    total_ns     avg_ns');
    for (const r of rows)
        console.log(String(r.LINE_NUM).padStart(4) + String(r.COLUMN_NUM).padStart(5) +
            String(r.COUNTER).padStart(9) + String(r.TOTAL_NS).padStart(12) +
            String(r.AVG_NS).padStart(11));

    await conn.detach();
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
