//! parser_errors.rs — driving the SQL parser and reading its error reports.
//!
//! The rsfbclient twin of ../cpp/parser_errors.cpp (see
//! ../../grammar-and-parser.md).  rsfbclient exposes no prepare-only step —
//! each query() prepares and executes in one go, like the node-firebird
//! twin — and no IStatement metadata, so the C++ sample's "input params /
//! output columns" report is out of reach.  What survives intact is the
//! parser itself: the status vector travels back through libfbclient
//! unchanged and FbError carries every line of it, so the same
//! "Token unknown - line N, column M" and "Column unknown ... At line N,
//! column M" reports print here byte for byte.  Successful parses are
//! shown by the rows they return; the `?` placeholder demonstrates a
//! dynamic-SQL parameter typed by the parser's semantic pass.
//!
//! Read-only against the stock employee database.
//!
//! Run (see ../README.md):  cargo run --bin parser_errors

use rsfbclient::{prelude::*, FbError, Row, SimpleTransaction};

fn attach_employee() -> Result<rsfbclient::SimpleConnection, FbError> {
    let conn = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(std::env::args().nth(1).unwrap_or_else(|| "employee".into()))
        .user(fb_handson_rust::user())
        .pass(fb_handson_rust::password())
        .connect()?;
    Ok(conn.into())
}

// Feed one string to the parser; report either the rows the parsed
// statement produced or the status vector the parser sent back.
fn try_query(tr: &mut SimpleTransaction, sql: &str, param: Option<i64>) {
    println!("---- {}", sql.replace('\n', "\\n"));
    let result: Result<Vec<Row>, FbError> = match param {
        Some(p) => tr.query(sql, (p,)),
        None => tr.query(sql, ()),
    };
    match result {
        Ok(rows) => {
            let cols: Vec<String> = rows
                .first()
                .map(|r| {
                    r.cols
                        .iter()
                        .map(|c| format!("{}={}", c.name, fb_handson_rust::col_text(c)))
                        .collect()
                })
                .unwrap_or_default();
            println!("  parsed OK: {} row(s), first: {}", rows.len(), cols.join(", "));
        }
        Err(e) => println!("  prepare failed:\n    {}", e.to_string().replace('\n', "\n    ")),
    }
}

fn main() -> Result<(), FbError> {
    let mut conn = attach_employee()?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    // 1. Dynamic SQL: the `?` becomes a typed parameter (the parser builds
    //    the node tree, the semantic pass resolves its type from EMP_NO).
    try_query(
        &mut tr,
        "SELECT first_name FROM employee WHERE emp_no = ?",
        Some(2),
    );

    // 2. One token, two grammatical roles: FIRST as row-limit clause...
    try_query(&mut tr, "SELECT FIRST 1 emp_no FROM employee", None);
    // ...and FIRST as an ordinary identifier (non-reserved keyword).
    try_query(
        &mut tr,
        "SELECT first FROM (SELECT 1 AS first FROM rdb$database)",
        None,
    );

    // 3. Syntax errors — the lexer/parser reports the offending token with
    //    its exact line and column.
    try_query(&mut tr, "SELEC 1 FROM rdb$database", None);
    try_query(&mut tr, "SELECT emp_no\nFROM employee\nWHERE ORDER BY 1", None);

    // 4. Semantic error — position tracking survives past the parse into
    //    the DSQL pass.
    try_query(&mut tr, "SELECT frst_name\nFROM employee", None);

    tr.commit()?;
    println!("done.");
    Ok(())
}
