//! migration.rs — the migration-and-interoperability scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/migration.cpp.  The C++ sample reads the
//! DESCRIBED wire type codes off IMessageMetadata; rsfbclient has no such
//! API — it maps every column it recognizes onto its small SqlType enum
//! (Text / Integer / Floating / Timestamp / Boolean / Binary / Null) and
//! returns an error for everything else.  That coarse mapping IS the
//! migration story: this sample probes the same TYPE_PROBE table column
//! by column and prints what each Firebird type actually arrives as —
//! including the ones (INT128, NUMERIC(38,8), DECFLOAT, TIMESTAMP WITH
//! TIME ZONE) that only survive the trip as a server-side CAST to
//! VARCHAR, the fallback every generic "select and copy strings" ETL
//! ends up using.  See ../../migration-and-interoperability.md.
//!
//! Run (see ../README.md):  cargo run --bin migration

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, Row, SimpleTransaction, SqlType};

fn type_name(t: u32) -> &'static str {
    match t {
        452 => "SQL_TEXT (CHAR)",
        448 => "SQL_VARYING (VARCHAR)",
        500 => "SQL_SHORT",
        496 => "SQL_LONG",
        580 => "SQL_INT64",
        32752 => "SQL_INT128",
        480 => "SQL_DOUBLE",
        32760 => "SQL_DEC16 (DECFLOAT 16)",
        32762 => "SQL_DEC34 (DECFLOAT 34)",
        510 => "SQL_TIMESTAMP",
        32754 => "SQL_TIMESTAMP_TZ",
        32764 => "SQL_BOOLEAN",
        520 => "SQL_BLOB",
        _ => "?",
    }
}

fn materialized(col: &rsfbclient::Column) -> String {
    let value = match &col.value {
        SqlType::Text(s) => format!("Text      {:?}", s),
        SqlType::Integer(i) => format!("Integer   {}", i),
        SqlType::Floating(f) => format!("Floating  {}", f),
        SqlType::Timestamp(t) => format!("Timestamp {}", t),
        SqlType::Boolean(b) => format!("Boolean   {}", b),
        SqlType::Binary(b) => format!("Binary    {} bytes", b.len()),
        SqlType::Null => "Null".to_string(),
    };
    format!("{:<28} (wire type {} {})", value, col.raw_type, type_name(col.raw_type))
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("migration")?;
    let mut tra = SimpleTransaction::new(&mut conn, Default::default())?;

    let _ = tra.execute("DROP TABLE TYPE_PROBE", ());
    tra.execute(
        "CREATE TABLE TYPE_PROBE (\
           C_INT128 INT128,\
           C_NUM    NUMERIC(38,8),\
           C_DEC    DECFLOAT(34),\
           C_TSTZ   TIMESTAMP WITH TIME ZONE,\
           C_BOOL   BOOLEAN,\
           C_UUID   CHAR(16) CHARACTER SET OCTETS,\
           C_VC     VARCHAR(20))",
        (),
    )?;
    tra.commit_retaining()?;
    // Fixed UUID (the C++ twin uses GEN_UUID()): deterministic bytes make
    // the driver's behaviour below reproducible.
    tra.execute(
        "INSERT INTO TYPE_PROBE VALUES (\
           170141183460469231731687303715884105727,\
           123456789012345678901234567890.12345678,\
           1.234567890123456789012345678901234E+10,\
           TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',\
           TRUE, CHAR_TO_UUID('E9E9E9E9-E9E9-E9E9-A9E9-E9E9E9E9E9E9'),\
           'naïve ütf8 text')",
        (),
    )?;
    tra.commit_retaining()?;

    // -- 1. what rsfbclient materializes, column by column --------------
    println!("what rsfbclient materializes, column by column:\n");
    for col in ["C_INT128", "C_NUM", "C_DEC", "C_TSTZ", "C_BOOL", "C_UUID", "C_VC"] {
        let fetched: Result<Option<Row>, FbError> =
            tra.query_first(&format!("SELECT {} FROM TYPE_PROBE", col), ());
        match fetched {
            Ok(Some(row)) => println!("  {:<8} -> {}", col, materialized(&row.cols[0])),
            Ok(None) => println!("  {:<8} -> (no rows)", col),
            Err(e) => println!(
                "  {:<8} -> ERROR: {}",
                col,
                e.to_string().lines().next().unwrap_or("")
            ),
        }
    }
    println!(
        "\n  (INT128, NUMERIC(38,8), DECFLOAT and TIMESTAMP WITH TIME ZONE never\n\
         \x20  leave the server: rsfbclient rejects their wire codes at prepare\n\
         \x20  time.  CHAR OCTETS arrives, but as Text — the mandatory charset\n\
         \x20  decode chokes on the raw bytes.)"
    );

    // The scaled types the driver CAN reach get flattened instead:
    let row: Option<Row> =
        tra.query_first("SELECT CAST(12345.6789 AS NUMERIC(18,4)) FROM RDB$DATABASE", ())?;
    println!(
        "\n  NUMERIC(18,4) 12345.6789 -> {}\n\
         \x20  (declared SQL_INT64 scale -4; the driver has the client library\n\
         \x20  coerce it to DOUBLE — lossy past 2^53, see numerics.rs)",
        materialized(&row.unwrap().cols[0])
    );

    // -- 2. the text face: engine-rendered strings, one per column ------
    println!("\nsame row with every column CAST to VARCHAR on the server:\n");
    let (i128s, nums, decs, tstzs, bools, vcs, uuids): (String, String, String, String, String, String, String) = tra
        .query_first(
            "SELECT CAST(C_INT128 AS VARCHAR(50)), CAST(C_NUM AS VARCHAR(50)), \
                    CAST(C_DEC AS VARCHAR(50)), CAST(C_TSTZ AS VARCHAR(60)), \
                    CAST(C_BOOL AS VARCHAR(10)), CAST(C_VC AS VARCHAR(20)), \
                    CAST(UUID_TO_CHAR(C_UUID) AS VARCHAR(40)) \
             FROM TYPE_PROBE",
            (),
        )?
        .unwrap();
    println!("  {:<8} = {}", "C_INT128", i128s);
    println!("  {:<8} = {}", "C_NUM", nums);
    println!("  {:<8} = {}", "C_DEC", decs);
    println!("  {:<8} = {}", "C_TSTZ", tstzs);
    println!("  {:<8} = {}", "C_BOOL", bools);
    println!("  {:<8} = {}", "C_VC", vcs);
    println!("  {:<8} = {}   (rendered via UUID_TO_CHAR)", "C_UUID", uuids);

    tra.commit()?;
    println!("\ndone.");
    Ok(())
}
