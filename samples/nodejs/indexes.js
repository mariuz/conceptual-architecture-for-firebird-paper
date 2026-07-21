//
// indexes.js — companion to ../../indexing-and-full-text-search.md
//
// The JS twin of ../cpp/indexes.cpp.  node-firebird's public API never asks
// the server for a plan, but its internal Connection.prepare() takes a
// `plan` flag that adds isc_info_sql_get_plan to the prepare-info request —
// so we reach one level down and print the same optimizer decisions through
// the pure-JavaScript wire protocol: expression, partial and descending
// indexes chosen, two indexes bitmap-OR-combined, and CONTAINING going
// NATURAL (the no-native-full-text gap).
//
// Run: node indexes.js
//
'use strict';

const { attachOrCreate } = require('./common');

const DB = '/tmp/fbhandson/indexes_js.fdb';

// Prepare via the driver's internal API to get statement.plan, then free.
function plan(tx, sql) {
    return new Promise((resolve, reject) => {
        const cnx = tx.tx.connection;
        cnx.prepare(tx.tx, sql, true, (err, st) => {
            if (err) return reject(err);
            const p = (st.plan || '(no plan)').trim();
            cnx.dropStatement(st, () => resolve(p));
        });
    });
}

(async () => {
    const conn = await attachOrCreate({ database: DB });
    await conn.query('recreate table doc (' +
        ' id integer, title varchar(60), status varchar(10), num integer)');
    await conn.query(
        `execute block as declare i integer = 0; begin
           while (i < 3000) do begin
             insert into doc values (:i, 'Title ' || :i,
               iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100));
             i = i + 1;
           end
         end`);
    await conn.query('create descending index doc_id_desc on doc (id)');
    await conn.query('create index doc_upper_title on doc computed by (upper(title))');
    await conn.query("create index doc_active on doc (status) where status = 'active'");
    await conn.query('create index doc_num on doc (num)');
    console.log('3000 rows; indexes: descending, expression, partial, plain\n');

    const tx = await conn.transaction();
    for (const sql of [
        "select id from doc where upper(title) = 'TITLE 5'",
        "select id from doc where status = 'active'",
        'select first 1 id from doc order by id desc',
        'select id from doc where num = 42 or id = 7',
        "select id from doc where title containing 'itle 12'",
    ])
        console.log(`${sql}\n${await plan(tx, sql)}\n`);
    await tx.rollback();

    const [r] = await conn.query(
        "select count(*) c from doc where title containing 'itle 12'");
    console.log(`CONTAINING is correct but unindexed: matched ${r.C} rows by scanning all 3000`);
    await conn.detach();
    console.log('done.');
})().catch(e => { console.error('ERR:', e.message); process.exit(1); });
