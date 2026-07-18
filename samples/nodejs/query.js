//
// query.js — connect to a Firebird server over the wire protocol from Node.js
//
// Uses node-firebird (https://github.com/hgourvest/node-firebird), a pure
// JavaScript implementation of the Firebird wire protocol: it opens the TCP
// socket itself, performs the op_connect handshake, authenticates with
// Srp/Srp256 and speaks XDR-encoded protocol packets — no fbclient library
// involved. Compare samples/client_test.cpp, which reaches the same server
// through the official C++ OO API (fbclient's Remote provider).
//
// Run:  node query.js [host] [port] [database] [user] [password]
//
const Firebird = require('node-firebird');

const options = {
    host: process.argv[2] || 'localhost',
    port: Number(process.argv[3]) || 3050,
    database: process.argv[4] || 'employee',
    user: process.argv[5] || 'SYSDBA',
    password: process.argv[6] || 'masterkey',
    // node-firebird picks the strongest authentication it supports from the
    // plugin list the server offers (Srp256, then Srp, then Legacy_Auth).
    //
    // The stock employee.fdb predates Unicode and has database charset NONE;
    // with the driver default (UTF8) node-firebird 2.11 mis-sizes NONE-charset
    // VARCHARs (length/4) and raises a spurious "string right truncation".
    encoding: 'NONE',
};

Firebird.attach(options, (err, db) => {
    if (err) throw err;

    db.query(
        `select rdb$get_context('SYSTEM', 'ENGINE_VERSION') as engine,
                rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL') as protocol,
                rdb$get_context('SYSTEM', 'WIRE_CRYPT_PLUGIN') as wire_crypt,
                current_user as who
           from rdb$database`,
        (err, rows) => {
            if (err) { db.detach(); throw err; }
            const r = rows[0];
            console.log('engine version :', r.ENGINE.toString().trim());
            console.log('protocol       :', r.PROTOCOL.toString().trim());
            console.log('wire crypt     :', r.WIRE_CRYPT ? r.WIRE_CRYPT.toString().trim() : '(none)');
            console.log('authenticated  :', r.WHO.toString().trim());

            db.query(
                'select first 3 emp_no, first_name, last_name from employee order by emp_no',
                (err, rows) => {
                    db.detach();
                    if (err) throw err;
                    for (const e of rows)
                        console.log(`employee ${e.EMP_NO}: ${e.FIRST_NAME.trim()} ${e.LAST_NAME.trim()}`);
                });
        });
});
