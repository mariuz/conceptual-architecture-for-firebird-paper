//
// windows.js — companion sample for ../../aggregate-and-window-functions.md.
//
// The JavaScript twin of ../cpp/windows.cpp: the same six-row sales table
// and the same flagship analytics through node-firebird.  Unlike the FB4
// types in the other samples, everything here decodes cleanly — window and
// aggregate results are ordinary INT64/DOUBLE/VARCHAR messages, so a
// pure-JS driver handles the full analytical surface (NUMERIC(10,2)
// arrives as a JS number via its scaled int64; LISTAGG is CAST to VARCHAR
// because it is otherwise a BLOB).
//
// Run:  node windows.js
//
'use strict';

const { attachOrCreate, s } = require('./common.js');

const dump = rows => {
    for (const r of rows)
        console.log('  ' + Object.entries(r)
            .map(([k, v]) => `${k.toLowerCase()}=${s(v)}`).join('  '));
};

(async () => {
    const db = await attachOrCreate({
        database: '/tmp/fbhandson/windows.fdb',
        encoding: 'UTF8',
    });
    try {
        try { await db.query('DROP TABLE sales'); } catch (e) {}
        await db.query('CREATE TABLE sales (id INT PRIMARY KEY,'
            + ' region VARCHAR(10), amount NUMERIC(10,2))');
        const rows = [[1, 'East', 100], [2, 'East', 200], [3, 'East', 150],
                      [4, 'West', 300], [5, 'West', 250], [6, 'West', 400]];
        for (const r of rows)
            await db.query('INSERT INTO sales VALUES (?,?,?)', r);

        console.log('window functions:');
        dump(await db.query(
            'SELECT region, amount,'
            + ' ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount) AS rn,'
            + ' RANK() OVER (ORDER BY amount DESC) AS overall_rank,'
            + ' SUM(amount) OVER (PARTITION BY region ORDER BY id'
            + '   ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_total,'
            + ' LAG(amount) OVER (PARTITION BY region ORDER BY id) AS prev_amount'
            + ' FROM sales'));

        console.log('\naggregates (FILTER / LISTAGG / STDDEV_POP):');
        dump(await db.query(
            'SELECT region, COUNT(*) AS n,'
            + ' COUNT(*) FILTER (WHERE amount > 150) AS big_sales,'
            + " CAST(LISTAGG(amount, ',') WITHIN GROUP (ORDER BY amount)"
            + '   AS VARCHAR(60)) AS amounts,'
            + ' CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev'
            + ' FROM sales GROUP BY region'));

        console.log('\nmedian and hypothetical rank of a 175 sale:');
        dump(await db.query(
            'SELECT region,'
            + ' PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median,'
            + ' RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175'
            + ' FROM sales GROUP BY region'));

        console.log('\nFB6 frame EXCLUDE CURRENT ROW (neighbour average):');
        dump(await db.query(
            'SELECT id, amount,'
            + ' CAST(AVG(amount) OVER (ORDER BY id'
            + '   ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING'
            + '   EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg'
            + ' FROM sales'));
    } finally {
        await db.detach();
    }
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
