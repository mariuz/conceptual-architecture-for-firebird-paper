//
// security.js — the security layers seen through the pure-JS driver.
// Companion to ../../security-architecture.md.
//
// Same observations as the C++ sample's read-only parts — who am I
// (MON$AUTH_METHOD / MON$WIRE_CRYPT_PLUGIN), SEC$USERS, and a failed
// login's error chain — but through a different client, which makes the
// negotiation visible: node-firebird ends up on Arc4 wire encryption where
// fbclient negotiates ChaCha64, against the very same server.
// (The user/role/system-privilege demo lives in the C++ sample.)
//
'use strict';

const { attach, s } = require('./common');

async function main() {
    const c = await attach();               // employee, read-only use

    // 1. Layers 1+2 for THIS attachment, as the server recorded them.
    const me = (await c.query(
        `select trim(mon$user) as who, mon$auth_method as auth,
                mon$wire_crypt_plugin as wire, mon$remote_protocol as proto
           from mon$attachments
          where mon$attachment_id = current_connection`))[0];
    console.log(`who am I : user=${s(me.WHO)} auth=${s(me.AUTH)}` +
        ` wirecrypt=${s(me.WIRE)} protocol=${s(me.PROTO)}`);

    // 2. The security database, through the SEC$USERS virtual view.
    const users = await c.query(
        'select trim(sec$user_name) as u, trim(sec$plugin) as p, sec$admin as a' +
        '  from sec$users order by 1');
    console.log('SEC$USERS:');
    for (const r of users)
        console.log(`    ${s(r.U).padEnd(16)} plugin=${s(r.P).padEnd(8)} admin=${r.A}`);

    await c.detach();

    // 3. A failed login and its error chain (SQLSTATE 28000, gds 335544472).
    try {
        await attach({ password: 'definitely-wrong' });
        console.log('!! unexpected: login succeeded');
    } catch (err) {
        console.log('failed login produces:');
        console.log(`    ${err.message.replace(/\s+/g, ' ').trim()} (gdscode ${err.gdscode})`);
    }
    console.log('done.');
}

main().catch(err => { console.error('ERR:', err.message); process.exit(1); });
