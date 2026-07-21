//
// services.js — the Services API through node-firebird's pure-JS driver.
// Companion to ../../services-api.md.
//
// Passing { manager: true } to Firebird.attach() sends op_service_attach
// instead of op_attach and yields a ServiceManager (lib/wire/service.js)
// whose methods build the SPB tag-by-tag — backup() writes
// isc_action_svc_backup + isc_spb_dbname + isc_spb_bkp_file exactly as the
// C++ sample's IXpbBuilder does.  Verbose output arrives as a Readable
// stream whose _read() issues one op_service_info(isc_info_svc_line) per
// line: the 1 KB ring-buffer polling loop dressed up as Node backpressure —
// stop reading the stream and the server-side gbak thread blocks.
//
'use strict';

const { Firebird, DEFAULTS } = require('./common');

const DB  = '/tmp/fbhandson/services.fdb';   // server paths, as always
const BK  = '/tmp/fbhandson/services_js.fbk';

const svcAttach = () => new Promise((res, rej) =>
    Firebird.attach({ ...DEFAULTS, manager: true }, (e, svc) => e ? rej(e) : res(svc)));

function main(svc) {
    // 1. Information request: server facts, no action started.
    svc.getFbserverInfos({ fbversion: true, fbimplementation: true }, {},
        (err, info) => {
            if (err) throw err;
            console.log('server version :', info.fbversion);
            console.log('implementation :', info.fbimplementation);

            // 2. action_backup on the server: BURP_main on a service thread.
            svc.backup({ database: DB, files: BK, verbose: true },
                (err, stream) => {
                    if (err) throw err;
                    let lines = 0, last = '';
                    stream.on('data', line => { lines++; last = String(line).trim(); });
                    stream.on('end', () => {
                        console.log(`backup done: ${lines} gbak lines streamed`);
                        console.log(`last line   : ${last}`);
                        console.log(`(${BK} was written on the SERVER, by the server's user)`);
                        svc.detach(() => console.log('service detached. done.'));
                    });
                });
        });
}

svcAttach()
    .then(main)
    .catch(err => { console.error('ERR:', err.message || err); process.exit(1); });
