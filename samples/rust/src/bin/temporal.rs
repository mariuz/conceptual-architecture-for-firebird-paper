//! temporal.rs — temporal types and time zones through rsfbclient.
//!
//! The rsfbclient twin of ../cpp/temporal.cpp.  The C++ sample decodes the
//! raw wire struct ISC_TIMESTAMP_TZ — a UTC instant plus a 2-byte zone id.
//! rsfbclient has no type for that struct at all: a zoneless TIMESTAMP
//! arrives as chrono::NaiveDateTime (a type that, true to the document,
//! carries *no* zone), and fetching TIMESTAMP WITH TIME ZONE fails with
//! "Unsupported column type (32754)" — the driver's honest refusal, where
//! node-firebird silently dropped the zone.  Everything zone-flavoured
//! therefore runs server-side: EXTRACT(TIMEZONE_NAME), CAST to VARCHAR,
//! AT TIME ZONE across a DST boundary, and SET TIME ZONE for the session —
//! plain SQL that works from any driver.
//! See ../../temporal-and-time-zones.md.
//!
//! Run (see ../README.md):  cargo run --bin temporal

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, Row, SimpleTransaction, SqlType};

const NY: &str = "TIMESTAMP '2026-07-18 12:00:00 America/New_York'";

fn main() -> Result<(), FbError> {
    let mut conn = connect("temporal")?;
    let mut tra = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    // Fetch a single value rendered by the server's own text conversion.
    macro_rules! text {
        ($sql:expr) => {{
            let v: Option<(String,)> = tra.query_first($sql, ())?;
            v.unwrap().0
        }};
    }

    // -- 1. A zoneless TIMESTAMP arrives as chrono::NaiveDateTime — the
    //       Rust type itself says "naive": no zone anywhere in the value.
    let plain: Option<Row> = tra.query_first(
        "SELECT TIMESTAMP '2026-07-18 12:00:00' FROM RDB$DATABASE",
        (),
    )?;
    match &plain.unwrap().cols[0].value {
        SqlType::Timestamp(t) => println!(
            "zoneless TIMESTAMP  : {}  <- chrono::NaiveDateTime, no zone in the type",
            t
        ),
        other => println!("zoneless TIMESTAMP  : unexpected variant {:?}", other),
    }

    // -- 2. TIMESTAMP WITH TIME ZONE does not fit any SqlType variant:
    //       the raw fetch fails outright (32754 = SQL_TIMESTAMP_TZ).  The
    //       C++ twin decodes this wire struct into instant + named zone.
    match tra.query_first::<(), Row>(&format!("SELECT {} FROM RDB$DATABASE", NY), ()) {
        Ok(_) => println!("raw TZ fetch        : fetched (unexpected on this driver version)"),
        Err(e) => println!("raw TZ fetch        : FAILED: {}  <- honest driver gap", e),
    }

    // -- The zone survives server-side: Firebird remembers where the value
    //    was, and the engine's text conversion can hand it to any driver.
    println!(
        "zone, server-side   : {}",
        text!(&format!("SELECT EXTRACT(TIMEZONE_NAME FROM {}) FROM RDB$DATABASE", NY))
    );
    println!(
        "text, server-side   : {}",
        text!(&format!("SELECT CAST({} AS VARCHAR(60)) FROM RDB$DATABASE", NY))
    );

    // -- 3. AT TIME ZONE across a DST boundary: the same New York wall
    //       time lands differently in UTC in January (EST) and July (EDT).
    println!(
        "\nNY 12:00 in UTC, winter: {}",
        text!(
            "SELECT CAST(TIMESTAMP '2026-01-18 12:00:00 America/New_York' \
             AT TIME ZONE 'Etc/UTC' AS VARCHAR(60)) FROM RDB$DATABASE"
        )
    );
    println!(
        "NY 12:00 in UTC, summer: {}",
        text!(
            "SELECT CAST(TIMESTAMP '2026-07-18 12:00:00 America/New_York' \
             AT TIME ZONE 'Etc/UTC' AS VARCHAR(60)) FROM RDB$DATABASE"
        )
    );

    // Equality is by UTC instant, regardless of zone spelling.
    println!(
        "10:00 -02:00 = 09:00 -03:00 ? {}",
        text!(
            "SELECT TRIM(IIF(TIME '10:00:00 -02:00' = TIME '09:00:00 -03:00', \
             'EQUAL', 'different')) FROM RDB$DATABASE"
        )
    );

    // -- 4. The session time zone governs "now" and zoneless conversions.
    //       SET TIME ZONE is session-level SQL — it works from any driver.
    println!(
        "\nsession zone: {}   CURRENT_TIMESTAMP: {}",
        text!(
            "SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE"
        ),
        text!(
            "SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE"
        )
    );
    tra.execute("SET TIME ZONE 'Asia/Tokyo'", ())?;
    println!(
        "session zone: {}     CURRENT_TIMESTAMP: {}",
        text!(
            "SELECT RDB$GET_CONTEXT('SYSTEM','SESSION_TIMEZONE') FROM RDB$DATABASE"
        ),
        text!(
            "SELECT CAST(CURRENT_TIMESTAMP AS VARCHAR(50)) FROM RDB$DATABASE"
        )
    );

    tra.commit()?;
    println!("\ndone.");
    Ok(())
}
