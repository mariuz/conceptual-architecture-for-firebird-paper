//! numerics.rs — the numeric-and-precision-arithmetic scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/numerics.cpp, with the twist the JS twin
//! also has: rsfbclient maps every scaled NUMERIC onto f64 (the native
//! backend rewrites the XSQLVAR to SQL_DOUBLE and lets the client library
//! convert), so the driver's "convenient" decoding re-introduces exactly
//! the binary rounding problem the type exists to avoid.  INT128 and
//! DECFLOAT do not arrive at all — their wire codes are rejected at
//! prepare time — so the honest route for them is the server-side CAST
//! to VARCHAR.  That coarseness is the teaching delta here.
//! See ../../numeric-and-precision-arithmetic.md.
//!
//! Run (see ../README.md):  cargo run --bin numerics

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, Row, SimpleTransaction};

fn first_line(e: &FbError) -> String {
    e.to_string().lines().next().unwrap_or("").to_string()
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("numerics")?;
    let mut tra = SimpleTransaction::new(&mut conn, Default::default())?;

    // -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3.
    let (dbl,): (f64,) = tra
        .query_first(
            "SELECT (CAST(0.1 AS DOUBLE PRECISION) + 0.2) - 0.3 FROM RDB$DATABASE",
            (),
        )?
        .unwrap();
    println!("(0.1+0.2)-0.3 in DOUBLE PRECISION : {:e}", dbl);
    let (dec,): (String,) = tra
        .query_first(
            "SELECT CAST((CAST(0.1 AS DECFLOAT(34)) + 0.2) - 0.3 AS VARCHAR(45)) FROM RDB$DATABASE",
            (),
        )?
        .unwrap();
    println!("(0.1+0.2)-0.3 in DECFLOAT(34)     : {}   (via server-side CAST)", dec);
    match tra.query_first::<(), (f64,)>("SELECT CAST(1 AS DECFLOAT(34)) FROM RDB$DATABASE", ()) {
        Ok(_) => println!("BUG: raw DECFLOAT fetch unexpectedly worked"),
        Err(e) => println!("raw DECFLOAT fetch                : ERROR: {}\n", first_line(&e)),
    }

    // -- 2. NUMERIC on the wire: a scaled SQL_INT64 — which rsfbclient
    //       flattens to f64 before we ever see it.
    let row: Option<Row> = tra.query_first(
        "SELECT CAST(12345.6789 AS NUMERIC(18,4)) FROM RDB$DATABASE",
        (),
    )?;
    let col = &row.unwrap().cols[0];
    let val = match col.value {
        rsfbclient::SqlType::Floating(f) => f,
        _ => unreachable!("NUMERIC arrives as Floating"),
    };
    println!(
        "NUMERIC(18,4) 12345.6789: wire type {} (SQL_INT64, scaled integer) —\n\
         \x20 but the driver coerces it to DOUBLE and hands us f64 {}",
        col.raw_type, val
    );

    // Where the flattening bites: a scaled integer past 2^53.
    let (cents,): (f64,) = tra
        .query_first(
            "SELECT CAST(90071992547409.93 AS NUMERIC(18,2)) FROM RDB$DATABASE",
            (),
        )?
        .unwrap();
    let (cents_text,): (String,) = tra
        .query_first(
            "SELECT CAST(CAST(90071992547409.93 AS NUMERIC(18,2)) AS VARCHAR(25)) FROM RDB$DATABASE",
            (),
        )?
        .unwrap();
    println!(
        "NUMERIC(18,2) as f64              : {}   <- off by a cent (raw int 9007199254740993 > 2^53)",
        cents
    );
    println!(
        "NUMERIC(18,2) server text         : {}   <- the value the server actually stores\n",
        cents_text
    );

    // -- 3. INT128: the full range, and one step past it.
    let (max,): (String,) = tra
        .query_first(
            "SELECT CAST(CAST(170141183460469231731687303715884105727 AS INT128) AS VARCHAR(50)) \
             FROM RDB$DATABASE",
            (),
        )?
        .unwrap();
    println!("INT128 max  : {}   (via CAST — the raw type never reaches the driver)", max);
    match tra.query_first::<(), (String,)>(
        "SELECT CAST(CAST(170141183460469231731687303715884105727 AS INT128) + 1 AS VARCHAR(50)) \
         FROM RDB$DATABASE",
        (),
    ) {
        Ok(_) => println!("BUG: overflow not detected"),
        Err(e) => println!("INT128 max+1: {}\n", first_line(&e)),
    }

    // -- 4. DECFLOAT division by zero: trapped by default, Infinity once
    //       the session's traps are cleared.
    match tra.query_first::<(), (String,)>(
        "SELECT CAST(CAST(1 AS DECFLOAT(16)) / 0 AS VARCHAR(20)) FROM RDB$DATABASE",
        (),
    ) {
        Ok(_) => println!("BUG: default trap did not fire"),
        Err(e) => println!("1/0 with default traps : {}", first_line(&e)),
    }
    tra.execute("SET DECFLOAT TRAPS TO", ())?; // clear all traps (session-level)
    let (inf,): (String,) = tra
        .query_first(
            "SELECT CAST(CAST(1 AS DECFLOAT(16)) / 0 AS VARCHAR(20)) FROM RDB$DATABASE",
            (),
        )?
        .unwrap();
    println!("1/0 with traps cleared : {}", inf);

    tra.commit()?;
    println!("\ndone.");
    Ok(())
}
