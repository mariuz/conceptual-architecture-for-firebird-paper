//
// trace.js — companion sample for ../../trace-and-audit.md
//
// The same fbtracemgr-less trace session as ../cpp/trace.cpp, in pure
// JavaScript: node-firebird's ServiceManager sends isc_action_svc_trace_start
// with the configuration text inline (isc_spb_trc_cfg — the driver option is
// named `configfile` but carries the text itself), streams the session's
// TraceLog back as a Readable, and a second service connection stops it.
//
'use strict';

const { Firebird, DEFAULTS, attach, attachOrCreate } = require('./common');

const DB = '/tmp/fbhandson/trace_js.fdb';

const TRACE_CFG = `database = ${DB}
{
  enabled = true
  log_connections = true
  log_statement_finish = true
  print_plan = true
  print_perf = true
  time_threshold = 0
}
`;

const svcAttach = () => new Promise((res, rej) =>
    Firebird.attach({ ...DEFAULTS, manager: true }, (e, svc) => e ? rej(e) : res(svc)));

(async () => {
    // The database to be observed must exist before the session starts.
    (await attachOrCreate({ database: DB, encoding: 'UTF8' })).detach();

    const svcA = await svcAttach();
    const stream = await svcA.startTraceAsync(
        { tracename: 'hands-on-js', configfile: TRACE_CFG });

    let sessionId = null;
    const done = new Promise((res, rej) => {
        stream.on('data', line => {
            line = String(line).trimEnd();
            console.log('[trace] ' + line);
            const m = line.match(/^Trace session ID (\d+) started/);
            if (m) sessionId = Number(m[1]);
        });
        stream.on('end', res);
        stream.on('error', rej);
    });

    // The observed side: one attachment, one marker statement.
    setTimeout(async () => {
        const conn = await attach({ database: DB, encoding: 'UTF8' });
        const r = await conn.query('SELECT COUNT(*) N FROM RDB$RELATIONS /* traced from JS! */');
        console.log(`[worker] marker query says: ${r[0].N}`);
        await conn.detach();

        // Stop the session from a second service connection.
        setTimeout(async () => {
            const svcB = await svcAttach();
            const stop = await svcB.stopTraceAsync({ traceid: sessionId });
            stop.on('data', l => console.log('[stop ] ' + String(l).trimEnd()));
            stop.on('end', () => svcB.detach(() => {}));
        }, 1500);
    }, 800);

    await done;
    await svcA.detachAsync();
    console.log('done.');
    process.exit(0);
})().catch(e => { console.error('FAILED:', e.message || e); process.exit(1); });
