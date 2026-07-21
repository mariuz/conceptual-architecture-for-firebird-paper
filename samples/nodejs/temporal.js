//
// temporal.js — companion sample for ../../temporal-and-time-zones.md.
//
// The JavaScript twin of ../cpp/temporal.cpp.  A JS Date is a bare UTC
// instant — the exact thing Firebird's WITH TIME ZONE type is *more* than.
// So node-firebird hands back correct instants and quietly drops the part
// this document is about: which zone the value was expressed in.
//
//   TIMESTAMP WITH TIME ZONE -> Date: instant right (16:00Z), zone gone.
//   TIME WITH TIME ZONE      -> Date pinned to 1970-01-01, zone gone.
//   zoneless TIMESTAMP       -> Date built in the *Node process's* local
//                               zone (TZ env), not the session zone.
//
// The zone survives only server-side: EXTRACT(TIMEZONE_NAME ...) or a
// CAST to VARCHAR recovers what the driver cannot carry.  SET TIME ZONE
// is session-level, so it persists across the driver's per-query
// transactions on one attachment.
//
// Run:  node temporal.js
//
'use strict';

const { attachOrCreate, s } = require('./common.js');

(async () => {
    const db = await attachOrCreate({
        database: '/tmp/fbhandson/temporal.fdb',
        encoding: 'UTF8',
    });
    const one = async sql => Object.values((await db.query(sql))[0])[0];
    try {
        const NY = "TIMESTAMP '2026-07-18 12:00:00 America/New_York'";

        const d = await one(`SELECT ${NY} FROM RDB$DATABASE`);
        console.log('raw fetch          :', d.toISOString(),
            ' <- correct instant (12:00 EDT = 16:00Z), zone name lost');
        console.log('zone, server-side  :',
            s(await one(`SELECT EXTRACT(TIMEZONE_NAME FROM ${NY}) FROM RDB$DATABASE`)));
        console.log('text, server-side  :',
            s(await one(`SELECT CAST(${NY} AS VARCHAR(60)) FROM RDB$DATABASE`)));

        const tt = await one("SELECT TIME '10:00:00 -02:00' FROM RDB$DATABASE");
        console.log('TIME WITH TIME ZONE:', tt.toISOString(),
            ' <- 12:00Z instant, date pinned to epoch, zone gone');

        // DST across the year: same New York wall time, different UTC instant.
        console.log('\nNY noon in UTC, winter:', s(await one(
            "SELECT CAST(TIMESTAMP '2026-01-18 12:00:00 America/New_York'" +
            " AT TIME ZONE 'Etc/UTC' AS VARCHAR(60)) FROM RDB$DATABASE")));
        console.log('NY noon in UTC, summer:', s(await one(
            "SELECT CAST(TIMESTAMP '2026-07-18 12:00:00 America/New_York'" +
            " AT TIME ZONE 'Etc/UTC' AS VARCHAR(60)) FROM RDB$DATABASE")));

        // The session zone: changed once, visible in later queries.
        console.log('\nsession zone before:',
            s(await one("SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE")));
        await db.query("SET TIME ZONE 'Asia/Tokyo'");
        console.log('session zone after :',
            s(await one("SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE")));
        console.log('CURRENT_TIMESTAMP  :',
            s(await one('SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(60)) FROM RDB$DATABASE')));
    } finally {
        await db.detach();
    }
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
