//
// architecture-comparison.js — the wire is the only way in for a pure-JS driver.
//
// Companion to ../../architecture-comparison.md and the JS twin of
// ../cpp/architecture_comparison.cpp.  The C++ sample reaches the engine two
// ways (Remote provider over TCP, Engine provider loaded into the process)
// because libfbclient carries the whole Y-valve/provider stack.  node-firebird
// implements the WIRE PROTOCOL ONLY, in JavaScript — there is no Y-valve, no
// provider chain, no engine to load in-process.  That asymmetry IS the
// architectural point: Firebird's embedded mode is a property of the native
// client library, not of the server, so a driver that reimplements only the
// network layer gets exactly the client-server half of the architecture.
//
'use strict';

const { attach, s } = require('./common');

(async () => {
    // The one path available: TCP to a server process.
    const conn = await attach({ database: 'employee' });

    const rows = await conn.query(
        "select rdb$get_context('SYSTEM', 'ENGINE_VERSION') as ver, " +
        "       rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL') as proto, " +
        "       a.mon$server_pid as spid " +
        "from mon$attachments a " +
        "where a.mon$attachment_id = current_connection");

    const r = rows[0];
    console.log('node-firebird (pure JS, wire protocol only):');
    console.log('    ENGINE_VERSION    :', s(r.VER));
    console.log('    NETWORK_PROTOCOL  :', s(r.PROTO));
    console.log(`    MON$SERVER_PID    : ${r.SPID}   (this node process is pid ${process.pid})`);
    await conn.detach();

    // And the way that does not exist: an embedded, in-process attach.
    // A local path is not a connection string the driver can act on by
    // itself — it still opens a TCP socket to options.host and merely asks
    // the SERVER to open that path.  With no native library there is nothing
    // that could load Engine14 into this process.
    console.log('\nThere is no embedded path:');
    console.log('    node-firebird speaks the Remote protocol and nothing else;');
    console.log('    a database path in options.database is always resolved by');
    console.log('    the server at options.host, never by this process.');
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
