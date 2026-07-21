//
// numerics.js — companion sample for ../../numeric-and-precision-arithmetic.md.
//
// The JavaScript twin of ../cpp/numerics.cpp, with a twist the C++ sample
// cannot show: JavaScript itself only has IEEE binary doubles, so the
// driver's "convenient" decoding of scaled NUMERIC into a JS number
// re-introduces exactly the rounding problem the type exists to avoid.
//
//   - DOUBLE residue of (0.1+0.2)-0.3 arrives intact (5.55e-17).
//   - NUMERIC(18,4) 12345.6789 -> JS number, fine (fits in 53 bits scaled).
//   - NUMERIC(18,2) 90071992547409.93 -> 90071992547409.92: the scaled
//     integer 9007199254740993 exceeds 2^53, so the driver's Number is off
//     by a cent.  The server-side CAST to VARCHAR shows the true value.
//   - DECFLOAT cannot be fetched raw at all (-804); CAST(... AS VARCHAR)
//     is the honest route, and shows 0.1+0.2-0.3 = 0 exactly.
//   - The default Division_by_zero trap raises 22012; after
//     SET DECFLOAT TRAPS TO (session-level, persists across queries on
//     this attachment) the same division returns Infinity.
//
// Run:  node numerics.js
//
'use strict';

const { attachOrCreate, s } = require('./common.js');

(async () => {
    const db = await attachOrCreate({
        database: '/tmp/fbhandson/numerics.fdb',
        encoding: 'UTF8',
    });
    const one = async sql => Object.values((await db.query(sql))[0])[0];
    try {
        console.log('(0.1+0.2)-0.3 in DOUBLE     :',
            await one('SELECT (CAST(0.1 AS DOUBLE PRECISION)+0.2)-0.3 FROM RDB$DATABASE'));
        console.log('(0.1+0.2)-0.3 in DECFLOAT   :',
            s(await one('SELECT CAST((CAST(0.1 AS DECFLOAT(34))+0.2)-0.3 AS VARCHAR(45)) FROM RDB$DATABASE')),
            ' (via server-side CAST; raw DECFLOAT fetch fails with -804)');

        console.log('\nNUMERIC(18,4) 12345.6789    :',
            await one('SELECT CAST(12345.6789 AS NUMERIC(18,4)) FROM RDB$DATABASE'),
            ' (JS number, exact — scaled int fits in 2^53)');
        const asNumber = await one('SELECT CAST(90071992547409.93 AS NUMERIC(18,2)) FROM RDB$DATABASE');
        const asText = s(await one('SELECT CAST(CAST(90071992547409.93 AS NUMERIC(18,2)) AS VARCHAR(25)) FROM RDB$DATABASE'));
        console.log('NUMERIC(18,2) as JS number  :', asNumber, ' <- off by a cent (raw int 9007199254740993 > 2^53)');
        console.log('NUMERIC(18,2) server text   :', asText, ' <- the value the server actually stores');

        try {
            await one('SELECT CAST(CAST(1 AS DECFLOAT(16))/0 AS VARCHAR(20)) FROM RDB$DATABASE');
            console.log('\nBUG: default trap did not fire');
        } catch (e) {
            console.log('\n1/0 with default traps      :', e.message.split('\n')[0]);
        }
        await db.query('SET DECFLOAT TRAPS TO');
        console.log('1/0 after SET DECFLOAT TRAPS TO :',
            s(await one('SELECT CAST(CAST(1 AS DECFLOAT(16))/0 AS VARCHAR(20)) FROM RDB$DATABASE')));
    } finally {
        await db.detach();
    }
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
