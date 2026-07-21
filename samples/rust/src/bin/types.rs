//! types.rs — the SQL type system as seen through rsfbclient.
//!
//! The rsfbclient twin of ../cpp/types.cpp.  The C++ sample reads the
//! SQL_* wire code of every column straight from IMessageMetadata; the
//! story here is what a driver *makes* of those codes.  rsfbclient folds
//! the whole wire onto seven SqlType variants — every integer width
//! becomes Integer(i64), every scaled NUMERIC and float Floating(f64),
//! CHAR and VARCHAR both Text — and the FB4 types that fit no variant
//! (INT128, DECFLOAT, the WITH TIME ZONE family) fail to fetch outright.
//! The reliable route for those is the same one the other twins use:
//! CAST to VARCHAR and let the server's own CVT rules do the rendering.
//! The domain's CHECK constraint fires server-side either way, surfacing
//! as a typed FbError::Sql with its sqlcode.
//! See ../../sql-dialect-and-types.md.
//!
//! Run (see ../README.md):  cargo run --bin types

use fb_handson_rust::{col_text, connect};
use rsfbclient::{prelude::*, FbError, Row, SimpleTransaction, SqlType};

/// The SqlType variant a column landed in, with its value.
fn variant(v: &SqlType) -> String {
    match v {
        SqlType::Text(s) => format!("Text({:?})", s),
        SqlType::Integer(i) => format!("Integer({}_i64)", i),
        SqlType::Floating(f) => format!("Floating({}_f64)", f),
        SqlType::Timestamp(t) => format!("Timestamp({})", t),
        SqlType::Boolean(b) => format!("Boolean({})", b),
        SqlType::Binary(b) => format!("Binary({} bytes)", b.len()),
        SqlType::Null => "Null".into(),
    }
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("types")?;
    let cfg = TransactionConfiguration::default();

    // -- 1. The coarse mapping: ten declared types, seven possible variants.
    //       SMALLINT/INTEGER/BIGINT all arrive as Integer(i64); NUMERIC and
    //       both floats as Floating(f64); CHAR and VARCHAR as Text.
    println!("declared type      -> SqlType variant the driver delivers");
    println!("-----------------     --------------------------------------");
    {
        let mut tra = SimpleTransaction::new(&mut conn, cfg)?;
        let decls = [
            "SMALLINT",
            "INTEGER",
            "BIGINT",
            "NUMERIC(9,2)",
            "DOUBLE PRECISION",
            "CHAR(3)",
            "VARCHAR(5)",
            "BOOLEAN",
            "TIMESTAMP",
            "INTEGER (NULL)",
        ];
        let row: Option<Row> = tra.query_first(
            "SELECT CAST(1 AS SMALLINT), CAST(2 AS INTEGER), CAST(3 AS BIGINT), \
             CAST(4.50 AS NUMERIC(9,2)), CAST(5.5 AS DOUBLE PRECISION), \
             CAST('abc' AS CHAR(3)), CAST('vc' AS VARCHAR(5)), \
             TRUE, LOCALTIMESTAMP, CAST(NULL AS INTEGER) \
             FROM RDB$DATABASE",
            (),
        )?;
        let row = row.unwrap();
        for (decl, col) in decls.iter().zip(&row.cols) {
            println!("{:<18} -> {}", decl, variant(&col.value));
        }
        tra.commit()?;
    }

    // -- Idempotent cleanup.  Each drop commits on its own: DDL is partly
    //    deferred to commit time, so DROP DOMAIN's dependency check must
    //    run after the table drop is actually committed.
    for sql in ["DROP TABLE showcase", "DROP DOMAIN d_email"] {
        let mut t = SimpleTransaction::new(&mut conn, cfg)?;
        if t.execute(sql, ()).is_ok() {
            t.commit()?;
        } else {
            t.rollback()?;
        }
    }

    // -- DDL: the same domain and showcase table as the C++ twin.
    {
        let mut ddl = SimpleTransaction::new(&mut conn, cfg)?;
        ddl.execute(
            "CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')",
            (),
        )?;
        ddl.execute(
            "CREATE TABLE showcase ( \
              flag  BOOLEAN, \
              big   INT128, \
              money DECFLOAT(34), \
              born  TIMESTAMP WITH TIME ZONE, \
              mail  d_email)",
            (),
        )?;
        ddl.commit()?;
    }

    let mut tra = SimpleTransaction::new(&mut conn, cfg)?;
    tra.execute(
        "INSERT INTO showcase VALUES ( \
          TRUE, \
          170141183460469231731687303715884105727, \
          0.1, \
          TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest', \
          'user@example.com')",
        (),
    )?;

    // -- 2. The domain's CHECK travels with the type, and the violation
    //       comes back as a *typed* error: FbError::Sql { code, msg }.
    match tra.execute("INSERT INTO showcase (mail) VALUES ('not-an-address')", ()) {
        Ok(_) => println!("\nBUG: domain CHECK did not fire"),
        Err(FbError::Sql { code, msg }) => println!(
            "\ndomain CHECK rejected 'not-an-address' (FbError::Sql, sqlcode {}):\n    {}",
            code,
            msg.lines().next().unwrap_or("")
        ),
        Err(e) => println!("\nunexpected error kind: {}", e),
    }

    // -- 3. The FB4 types fit none of the seven variants: the whole-row
    //       fetch dies on the first unfetchable column, and each column
    //       fails alone with its raw SQL_* wire code in the message
    //       (32752 = SQL_INT128, 32762 = SQL_DEC34, 32754 = SQL_TIMESTAMP_TZ).
    println!("\nSELECT * FROM showcase:");
    match tra.query::<(), Row>("SELECT * FROM showcase", ()) {
        Ok(_) => println!("    fetched (unexpected on this driver version)"),
        Err(e) => println!("    FAILED: {}", e),
    }

    println!("\ncolumn by column:");
    for col in ["flag", "mail", "big", "money", "born"] {
        match tra.query_first::<(), Row>(&format!("SELECT {} FROM showcase", col), ()) {
            Ok(Some(row)) => println!("  {:<5} -> {}", col, variant(&row.cols[0].value)),
            Ok(None) => println!("  {:<5} -> no row", col),
            Err(e) => println!("  {:<5} -> FETCH FAILED: {}", col, e),
        }
    }

    // -- 4. The reliable route: make the server render what the driver
    //       cannot carry — CAST to VARCHAR, the engine's own conversion.
    println!("\nserver-side CAST(... AS VARCHAR) — the reliable route:");
    let row: Option<Row> = tra.query_first(
        "SELECT CAST(big AS VARCHAR(45)) AS big, \
         CAST(money AS VARCHAR(45)) AS money, \
         CAST(born AS VARCHAR(60)) AS born \
         FROM showcase",
        (),
    )?;
    for col in &row.unwrap().cols {
        println!("  {:<5} -> {}", col.name.to_lowercase(), col_text(col));
    }

    tra.commit()?;
    println!("\ndone.");
    Ok(())
}
