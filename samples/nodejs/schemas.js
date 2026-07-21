//
// schemas.js — companion sample for ../../schemas-and-name-resolution.md.
//
// The JavaScript twin of ../cpp/schemas.cpp.  Nothing here needs special
// driver support — schemas are pure server-side name resolution — but one
// driver behaviour matters: node-firebird wraps every query() in its own
// short transaction, and SET SEARCH_PATH still carries over, because the
// search path is ATTACHMENT state, not transaction state.  The sample
// proves that by changing the path in one query() call and observing the
// changed resolution in the next.
//
// Run:  node schemas.js
//
'use strict';

const { attachOrCreate, s } = require('./common.js');

(async () => {
    const db = await attachOrCreate({
        database: '/tmp/fbhandson/schemas.fdb',
        encoding: 'UTF8',
    });
    const one = async sql => Object.values((await db.query(sql))[0])[0];
    try {
        // Idempotent setup (shared with the C++ sample).
        for (const stmt of ['DROP PROCEDURE APP.WHICH_ONE',
                'DROP TABLE PUBLIC.CUSTOMERS', 'DROP TABLE APP.CUSTOMERS',
                'DROP SCHEMA APP'])
            try { await db.query(stmt); } catch (e) {}
        await db.query('CREATE SCHEMA APP');
        await db.query('CREATE TABLE PUBLIC.CUSTOMERS (ID INT, ORIGIN VARCHAR(20))');
        await db.query('CREATE TABLE APP.CUSTOMERS    (ID INT, ORIGIN VARCHAR(20))');
        await db.query("INSERT INTO PUBLIC.CUSTOMERS VALUES (1, 'from PUBLIC')");
        await db.query("INSERT INTO APP.CUSTOMERS    VALUES (2, 'from APP')");

        console.log('search path (default)  :',
            s(await one("SELECT RDB$GET_CONTEXT('SYSTEM','SEARCH_PATH') FROM RDB$DATABASE")));
        console.log('unqualified CUSTOMERS  :',
            s(await one('SELECT ORIGIN FROM CUSTOMERS')));

        // The path is attachment state: set in one query(), effective in the next.
        await db.query('SET SEARCH_PATH TO APP, PUBLIC');
        console.log('after SET ... APP,PUBLIC:',
            s(await one('SELECT ORIGIN FROM CUSTOMERS')));

        await db.query('SET SEARCH_PATH TO APP');
        console.log('SET ... TO APP shows   :',
            s(await one("SELECT RDB$GET_CONTEXT('SYSTEM','SEARCH_PATH') FROM RDB$DATABASE")),
            ' (SYSTEM auto-appended)');

        // Stored code binds its own schema, immune to the session path.
        await db.query('SET SEARCH_PATH TO APP, PUBLIC');
        await db.query(
            'CREATE PROCEDURE WHICH_ONE RETURNS (SRC VARCHAR(20)) AS '
            + 'BEGIN SELECT ORIGIN FROM CUSTOMERS INTO :SRC; SUSPEND; END');
        await db.query('SET SEARCH_PATH TO PUBLIC');
        console.log('\npath now PUBLIC; direct:',
            s(await one('SELECT ORIGIN FROM CUSTOMERS')));
        console.log('APP.WHICH_ONE returns  :',
            s(await one('SELECT SRC FROM APP.WHICH_ONE')),
            ' <- still its own schema');
        console.log('dependency recorded    :', s(await one(
            "SELECT TRIM(RDB$DEPENDED_ON_SCHEMA_NAME) || '.' || TRIM(RDB$DEPENDED_ON_NAME)"
            + " FROM RDB$DEPENDENCIES WHERE RDB$DEPENDENT_NAME = 'WHICH_ONE'")));
    } finally {
        await db.detach();
    }
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
