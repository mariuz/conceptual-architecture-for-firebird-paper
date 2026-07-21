//
// embedded-demo.js — what a pure-JS driver can and cannot do about embedded.
//
// Companion to ../../embedded-architecture-comparison.md and the JS twin of
// ../cpp/embedded_demo.cpp.  The C++ sample loads the full engine into its
// own process (libEngine14.so appears in its memory map) and attaches in a
// few milliseconds with no server.  node-firebird CANNOT do that, and the
// reason is architectural, not a missing feature: Firebird's embedded mode
// is native engine code (Engine14) loaded through the native client's
// Y-valve.  A driver that reimplements the wire protocol in JavaScript has
// no native code to load — its only transport is the socket.
//
// So this twin does the half that exists — timing the remote attach — and
// documents the half that doesn't.
//
'use strict';

const { attach } = require('./common');

async function attachMs(overrides) {
    const t0 = process.hrtime.bigint();
    const conn = await attach(overrides);
    await conn.detach();
    return Number(process.hrtime.bigint() - t0) / 1e6;
}

(async () => {
    // The half that exists: attach over the wire (socket + SRP handshake).
    const runs = 5;
    await attachMs({ database: 'employee' });          // warm-up
    let total = 0;
    for (let i = 0; i < runs; i++)
        total += await attachMs({ database: 'employee' });
    console.log(`remote attach+detach avg over ${runs} runs: ` +
        `${(total / runs).toFixed(2)} ms  (socket + auth handshake, every time)`);

    // The half that doesn't: there is no embedded attach to time.
    console.log('\nembedded attach: not possible from a pure-JS driver.');
    console.log('    node-firebird ships no native code, so there is no way to');
    console.log('    load the Engine provider into the node process; a local');
    console.log("    path like '/tmp/fbhandson/x.fdb' would still be sent to a");
    console.log('    server for THAT process to open. Compare the C++ twin,');
    console.log('    where the same path makes this very process the engine.');
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
