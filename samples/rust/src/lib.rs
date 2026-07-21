//! Shared defaults for the Rust hands-on twins.
//!
//! These samples mirror the OO-API programs in ../cpp (and their fb-cpp and
//! node-firebird siblings) using rsfbclient
//! (<https://github.com/fernandobatels/rsfbclient>).  The library speaks two
//! dialects: the NATIVE backend loads/links the same libfbclient the C++
//! samples use, and the PURE RUST backend is an independent implementation
//! of the wire protocol.  The samples default to the native backend; the
//! ones that discuss the wire use both and say which.
//!
//! Like the other twins, everything runs against the local server with
//! scratch databases under /tmp/fbhandson (SYSDBA/masterkey, overridable
//! via ISC_USER / ISC_PASSWORD).

use rsfbclient::{charset, FbError, SqlType};

pub fn user() -> String {
    std::env::var("ISC_USER").unwrap_or_else(|_| "SYSDBA".into())
}

pub fn password() -> String {
    std::env::var("ISC_PASSWORD").unwrap_or_else(|_| "masterkey".into())
}

/// Database path for a topic's scratch database (a SERVER path).
pub fn db_path(topic: &str) -> String {
    std::env::args()
        .nth(1)
        .unwrap_or_else(|| format!("/tmp/fbhandson/{}_rust.fdb", topic))
}

/// Attach to the topic's scratch database over the remote protocol,
/// creating it if the first attach fails.  UTF8 connection charset, like
/// the C++ helper.
pub fn connect(topic: &str) -> Result<rsfbclient::SimpleConnection, FbError> {
    let path = db_path(topic);
    let builder = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(&path)
        .user(user())
        .pass(password())
        .charset(charset::UTF_8)
        .clone();
    let b2 = builder;
    match b2.connect() {
        Ok(c) => Ok(c.into()),
        Err(_) => Ok(b2.create_database()?.into()),
    }
}

/// Render one column of a row as text for display, NULLs as `<null>`.
pub fn col_text(col: &rsfbclient::Column) -> String {
    match &col.value {
        SqlType::Text(s) => s.clone(),
        SqlType::Integer(i) => i.to_string(),
        SqlType::Floating(f) => format!("{}", f),
        SqlType::Timestamp(t) => t.to_string(),
        SqlType::Boolean(b) => b.to_string(),
        SqlType::Binary(b) => format!("{} bytes", b.len()),
        SqlType::Null => "<null>".into(),
    }
}

/// Print rows fetched as `rsfbclient::Row` as an aligned table with a
/// header line — the Rust twin of the C++ helper's `Db::print`.
pub fn print_table(rows: &[rsfbclient::Row]) {
    if rows.is_empty() {
        println!("(no rows)");
        return;
    }
    let headers: Vec<String> = rows[0].cols.iter().map(|c| c.name.clone()).collect();
    let cells: Vec<Vec<String>> = rows
        .iter()
        .map(|r| r.cols.iter().map(col_text).collect())
        .collect();
    let mut widths: Vec<usize> = headers.iter().map(|h| h.len()).collect();
    for row in &cells {
        for (i, cell) in row.iter().enumerate() {
            widths[i] = widths[i].max(cell.len());
        }
    }
    let fmt_line = |row: &[String]| {
        row.iter()
            .zip(&widths)
            .map(|(cell, w)| format!("{:<1$}", cell, w))
            .collect::<Vec<_>>()
            .join("  ")
            .trim_end()
            .to_string()
    };
    println!("{}", fmt_line(&headers));
    println!(
        "{}",
        widths
            .iter()
            .map(|w| "-".repeat(*w))
            .collect::<Vec<_>>()
            .join("  ")
    );
    for row in &cells {
        println!("{}", fmt_line(row));
    }
}
