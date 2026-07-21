//
// stmt_cache.js — companion to ../../statement-cache.md
//
// The JavaScript twin of ../cpp/stmt_cache.cpp.  node-firebird cannot
// prepare without executing (each query() is allocate + prepare + execute
// + drop in one round trip), so every timing here includes execution — the
// six-way self-join is chosen to be expensive to compile and nearly free
// to run, so the compile cost still dominates and the cache hit/miss
// pattern shows through clearly.
//
'use strict';

const { attachOrCreate } = require('./common');

const DB = process.env.FB_DATABASE || '/tmp/fbhandson/stmt_cache.fdb';
const N = 100;
const HEAVY = 'SELECT COUNT(*) FROM t a'
    + ' JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id'
    + ' JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id'
    + ' JOIN t f ON e.id = f.id WHERE a.id > 0';

async function timed(label, verdict, fn) {
    const t0 = process.hrtime.bigint();
    await fn();
    const ms = Number(process.hrtime.bigint() - t0) / 1e6;
    console.log(`${label} ${N} queries: ${ms.toFixed(1).padStart(7)} ms`
        + `  (${(ms / N).toFixed(2)} ms/query) - ${verdict}`);
}

(async () => {
    const conn = await attachOrCreate({ database: DB, encoding: 'UTF8' });
    try {
        await conn.query('RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)');
        await conn.query('EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN'
            + ' WHILE (i <= 50) DO BEGIN INSERT INTO t VALUES (:i); i = i + 1;'
            + ' END END');
        await conn.query(HEAVY);   // warm the cache with the exact text

        await timed('1. identical text          ', 'hits', async () => {
            for (let i = 0; i < N; ++i) await conn.query(HEAVY);
        });

        await timed('2. + i trailing spaces     ', 'misses', async () => {
            for (let i = 0; i < N; ++i) await conn.query(HEAVY + ' '.repeat(i + 1));
        });

        await timed('3. identical text after DDL', 'misses', async () => {
            for (let i = 0; i < N; ++i) {
                await conn.query('RECREATE TABLE unrelated (x INT)'); // purge
                await conn.query(HEAVY);
            }
        });
    } finally {
        await conn.detach();
    }
})().catch(e => { console.error(e.message || e); process.exit(1); });
