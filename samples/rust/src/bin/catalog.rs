//! catalog.rs — companion to ../../catalog-bootstrap.md
//!
//! The rsfbclient twin of ../cpp/catalog.cpp: the catalog describing
//! itself, from client SQL.  On a freshly created database it shows:
//!   1. the fixed relation ids burned into relations.h declaration order
//!      (RDB$PAGES 0, RDB$DATABASE 1, RDB$FIELDS 2, RDB$RELATIONS 6);
//!   2. RDB$PAGES carrying its own pointer page — and the hdr_PAGES word
//!      on page 0 agreeing with it (the anti-recursion anchor), read
//!      straight out of the database file with std::fs;
//!   3. RDB$FORMATS empty while sixty-odd system relations with hundreds
//!      of columns are fully usable: their formats are compiled into
//!      libEngine (INI_init), not stored;
//!   4. a user table gaining RDB$FORMATS rows the moment DDL creates and
//!      alters it — user formats live in the catalog.
//!
//! Run (see ../README.md):  cargo run --bin catalog

use fb_handson_rust::{connect, db_path};
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn main() -> Result<(), FbError> {
    // A truly fresh database each run: drop it if it exists, recreate.
    connect("catalog")?.drop_database()?;
    let mut conn = connect("catalog")?;
    let local_file = db_path("catalog"); // same machine: the server's file

    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    println!("-- 1. fixed relation ids (relations.h declaration order) --");
    println!("  {:>2}  {}", "ID", "NAME");
    let ids: Vec<(i64, String)> = tr.query(
        "select rdb$relation_id, trim(rdb$relation_name) \
         from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1",
        (),
    )?;
    for (id, name) in ids {
        println!("  {:>2}  {}", id, name);
    }

    println!("\n-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --");
    println!("  {:>6}  {:>7}  {:>8}  {:>4}", "PAGE", "REL_ID", "SEQUENCE", "TYPE");
    let pages: Vec<(i64, i64, i64, i64)> = tr.query(
        "select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type \
         from rdb$pages where rdb$relation_id in (0, 6) \
         order by rdb$relation_id, rdb$page_type, rdb$page_number",
        (),
    )?;
    let mut pages_anchor: i64 = -1;
    for (page, rel, seq, ptype) in pages {
        if rel == 0 && ptype == 4 {
            pages_anchor = page;
        }
        println!("  {:>6}  {:>7}  {:>8}  {:>4}", page, rel, seq, ptype);
    }

    // The anchor that cuts the recursion: hdr_PAGES at byte 28 of page 0.
    match std::fs::read(&local_file) {
        Ok(bytes) if bytes.len() >= 32 => {
            let hdr_pages = u32::from_le_bytes([bytes[28], bytes[29], bytes[30], bytes[31]]);
            println!(
                "\nhdr_PAGES (page 0, offset 28) = {}  <- matches the (relation 0, type 4) row above{}",
                hdr_pages,
                if i64::from(hdr_pages) == pages_anchor { "" } else { " ...or should!" }
            );
        }
        _ => println!("\n(could not read {} directly — run on the server host to see hdr_PAGES)", local_file),
    }

    println!("\n-- 3. formats as code: zero stored formats, yet a full catalog --");
    let (formats, sys_rels, sys_fields): (i64, i64, i64) = tr
        .query_first(
            "select (select count(*) from rdb$formats), \
                    (select count(*) from rdb$relations where rdb$system_flag = 1), \
                    (select count(*) from rdb$relation_fields r join rdb$relations rel \
                       on r.rdb$relation_name = rel.rdb$relation_name \
                       and r.rdb$schema_name = rel.rdb$schema_name \
                     where rel.rdb$system_flag = 1) \
             from rdb$database",
            (),
        )?
        .unwrap();
    println!("  FORMATS_ROWS {}   SYS_RELATIONS {}   SYS_FIELDS {}", formats, sys_rels, sys_fields);
    tr.commit()?;

    println!("\n-- 4. user DDL writes formats into the catalog --");
    {
        let mut ddl = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        ddl.execute("create table t1 (a integer)", ())?;
        ddl.commit()?;
    }
    {
        let mut ddl = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        ddl.execute("alter table t1 add b varchar(10)", ())?;
        ddl.commit()?;
    }

    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    println!("  {:>6}  {:>6}  {}", "REL_ID", "FORMAT", "DESCRIPTOR_BYTES");
    let formats: Vec<(i64, i64, i64)> = tr.query(
        "select rdb$relation_id, rdb$format, octet_length(rdb$descriptor) \
         from rdb$formats order by rdb$relation_id, rdb$format",
        (),
    )?;
    for (rel, fmt, bytes) in formats {
        println!("  {:>6}  {:>6}  {:>16}", rel, fmt, bytes);
    }
    let (t1_id,): (i64,) = tr
        .query_first(
            "select rdb$relation_id from rdb$relations where rdb$relation_name = 'T1'",
            (),
        )?
        .unwrap();
    println!(
        "\n(relation id of T1: {} — the first user id; system tables still contribute no rows)",
        t1_id
    );
    tr.commit()?;

    println!("done.");
    Ok(())
}
