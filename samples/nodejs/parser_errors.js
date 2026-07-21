//
// parser_errors.js — companion to ../../grammar-and-parser.md
//
// The JavaScript twin of ../cpp/parser_errors.cpp.  node-firebird has no
// separate prepare() call — each query() allocates, prepares and executes a
// statement in one round trip — but the parser's status vector travels back
// over the wire unchanged, so the same "Token unknown - line N, column M"
// reports surface as the error message.  Successful parses are shown by the
// rows they return; the `?` placeholder demonstrates dynamic SQL parameters.
//
'use strict';

const { attach, s } = require('./common');

const CASES = [
    // dynamic SQL: the driver sends the ? and the value separately
    { sql: 'SELECT first_name FROM employee WHERE emp_no = ?', params: [2] },
    // FIRST as row-limit clause, then FIRST as a plain identifier
    { sql: 'SELECT FIRST 1 emp_no FROM employee' },
    { sql: 'SELECT first FROM (SELECT 1 AS first FROM rdb$database)' },
    // syntax errors: token + line/column from the parser
    { sql: 'SELEC 1 FROM rdb$database' },
    { sql: 'SELECT emp_no\nFROM employee\nWHERE ORDER BY 1' },
    // semantic error: position tracking survives into the DSQL pass
    { sql: 'SELECT frst_name\nFROM employee' },
];

(async () => {
    const conn = await attach();   // employee, read-only use
    try {
        for (const c of CASES) {
            console.log('----', c.sql.replace(/\n/g, '\\n'));
            try {
                const rows = await conn.query(c.sql, c.params || []);
                const first = rows[0] || {};
                const cols = Object.keys(first).map(k => `${k}=${s(first[k])}`);
                console.log(`  parsed OK: ${rows.length} row(s), first: ${cols.join(', ')}`);
            } catch (err) {
                console.log('  prepare failed:', err.message.trim().replace(/\n/g, '\n    '));
            }
        }
    } finally {
        await conn.detach();
    }
})().catch(e => { console.error(e); process.exit(1); });
