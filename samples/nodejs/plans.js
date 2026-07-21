//
// plans.js — companion to ../../query-optimizer-and-execution.md
//
// node-firebird has no IStatement::getPlan equivalent (the wire driver never
// requests isc_info_sql_explain_plan), so this twin uses the honest SQL-level
// route new in Firebird 6: the RDB$SQL.EXPLAIN package procedure, which
// *prepares* the given statement server-side and returns the detailed plan
// as rows — one row per record-source operator, never executing the query.
// The same experiment as ../cpp/plans.cpp: explain a SELECT, create an
// index, explain it again and watch the Full Scan become an index retrieval.
//
'use strict';

const { attachOrCreate, s } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/plans.fdb';

async function explain(conn, sql) {
    console.log('==', sql);
    const rows = await conn.query(
        'SELECT plan_line, "LEVEL" AS lvl, CAST(access_path AS VARCHAR(1024)) AS ap ' +
        'FROM rdb$sql.explain(CAST(? AS VARCHAR(512))) ORDER BY plan_line',
        [sql]);
    for (const r of rows) {
        // LEVEL gives the operator's depth; the blob itself may span several
        // lines (an index retrieval prints its Bitmap/Index children inline).
        const pad = '   ' + '    '.repeat(r.LVL);
        const text = (Buffer.isBuffer(r.AP) ? r.AP.toString('utf8') : String(r.AP))
            .replace(/\s+$/, '');
        console.log(pad + text.replace(/\n/g, '\n' + pad));
    }
    console.log();
}

(async () => {
    const conn = await attachOrCreate({ database: DB, encoding: 'UTF8' });
    try {
        // RECREATE drops the table with any indexes — idempotent setup.
        await conn.query('RECREATE TABLE emp (id INT NOT NULL PRIMARY KEY,'
            + ' dept_id INT, salary INT, name VARCHAR(20))');
        await conn.query(
            'EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN'
            + ' WHILE (i <= 2000) DO BEGIN'
            + "   INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,"
            + "       1000 + MOD(:i * 37, 500), 'emp ' || :i); i = i + 1;"
            + ' END END');

        await explain(conn, 'SELECT name FROM emp WHERE dept_id = 5');

        console.log('-- CREATE INDEX emp_dept ON emp (dept_id) --\n');
        await conn.query('CREATE INDEX emp_dept ON emp (dept_id)');

        await explain(conn, 'SELECT name FROM emp WHERE dept_id = 5');
        await explain(conn,
            'SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary');
    } finally {
        await conn.detach();
    }
})().catch(e => { console.error(e.message || e); process.exit(1); });
