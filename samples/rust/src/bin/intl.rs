//! intl.rs — the internationalization scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/intl.cpp: collations deciding equality
//! and order on the same three rows, per-column charsets, and the
//! connection charset (lc_ctype) driving transliteration.  rsfbclient's
//! builder `.charset(..)` IS the lc_ctype: the helper connects with UTF8,
//! and this sample adds a second connection with a hand-built
//! `Charset { on_firebird: "NONE", on_rust: None }`.  The driver always
//! decodes Text columns into a Rust String with that charset, so the raw
//! wire bytes show up indirectly but honestly: under UTF8 the String's
//! own bytes ARE the wire bytes (the decode is an identity check), and
//! under NONE the server passes the stored bytes through untransliterated
//! — the WIN1252 byte E9 then fails the driver's UTF-8 decode, which is
//! itself the proof that no server-side conversion happened.
//! See ../../internationalization.md.
//!
//! Run (see ../README.md):  cargo run --bin intl

use fb_handson_rust::{connect, db_path, password, user};
use rsfbclient::{prelude::*, Charset, FbError, SimpleTransaction};

fn hexdump(label: &str, s: &str) {
    print!("  {:<16} len={:2}  ", label, s.len());
    for b in s.bytes() {
        print!("{:02X} ", b);
    }
    println!("  {:?}", s);
}

fn main() -> Result<(), FbError> {
    let mut utf8 = connect("intl")?; // connection charset UTF8 (helper default)

    // Scratch table (idempotent) — same shape as the C++ sample.
    {
        let mut ddl = SimpleTransaction::new(&mut utf8, Default::default())?;
        let _ = ddl.execute("DROP TABLE t", ());
        ddl.execute(
            "CREATE TABLE t (\
               name_ci_ai VARCHAR(30) CHARACTER SET UTF8 COLLATE UNICODE_CI_AI,\
               name_bin   VARCHAR(30) CHARACTER SET UTF8 COLLATE UCS_BASIC,\
               name_win   VARCHAR(30) CHARACTER SET WIN1252)",
            (),
        )?;
        ddl.commit()?;
    }

    let mut tra = SimpleTransaction::new(&mut utf8, Default::default())?;
    for v in ["Café", "CAFE", "cafe"] {
        tra.execute(
            &format!("INSERT INTO t VALUES ('{v}','{v}','{v}')"),
            (),
        )?;
    }

    // -- 1. The collation, not the data, decides what "equal" means.
    let (ci,): (i64,) = tra
        .query_first("SELECT COUNT(*) FROM t WHERE name_ci_ai = 'cafe'", ())?
        .unwrap();
    let (bin,): (i64,) = tra
        .query_first("SELECT COUNT(*) FROM t WHERE name_bin = 'cafe'", ())?
        .unwrap();
    let (upper,): (String,) = tra
        .query_first("SELECT UPPER('café èñ ß') FROM RDB$DATABASE", ())?
        .unwrap();
    println!("rows matching 'cafe' with UNICODE_CI_AI : {}", ci);
    println!("rows matching 'cafe' with UCS_BASIC     : {}", bin);
    println!("UPPER('café èñ ß')                      : {}\n", upper);

    // Sorting differs too: CI_AI groups the spellings, UCS_BASIC is binary.
    let ci_sorted: Vec<(String,)> =
        tra.query("SELECT name_ci_ai FROM t ORDER BY name_ci_ai", ())?;
    print!("ORDER BY name_ci_ai: ");
    for (v,) in &ci_sorted {
        print!("{}  ", v);
    }
    let bin_sorted: Vec<(String,)> =
        tra.query("SELECT name_bin FROM t ORDER BY name_bin", ())?;
    print!("\nORDER BY name_bin  : ");
    for (v,) in &bin_sorted {
        print!("{}  ", v);
    }
    println!("   (binary: uppercase codepoints first)\n");
    tra.commit()?;

    // -- 2./3. Same stored WIN1252 'Café', two connection charsets.
    println!("SELECT name_win FROM t WHERE name_bin = 'Café' — same row, two connections:");
    {
        let mut t1 = SimpleTransaction::new(&mut utf8, Default::default())?;
        let (win,): (String,) = t1
            .query_first("SELECT name_win FROM t WHERE name_bin = 'Café'", ())?
            .unwrap();
        hexdump("lc_ctype=UTF8:", &win); // String bytes == wire bytes (UTF8 decode is identity)
        t1.commit()?;
    }

    // Second attachment with connection charset NONE: the server sends the
    // stored bytes as-is, and rsfbclient (no on_rust codec) checks them as
    // UTF-8 while building the String.
    let charset_none = Charset {
        on_firebird: "NONE",
        on_rust: None,
    };
    let mut none: rsfbclient::SimpleConnection = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(db_path("intl"))
        .user(user())
        .pass(password())
        .charset(charset_none)
        .connect()?
        .into();
    let mut t2 = SimpleTransaction::new(&mut none, Default::default())?;
    match t2.query_first::<(), (String,)>("SELECT name_win FROM t WHERE name_bin = 'Café'", ()) {
        Ok(Some((v,))) => hexdump("lc_ctype=NONE:", &v),
        Ok(None) => println!("  lc_ctype=NONE:   no row"),
        Err(e) => println!(
            "  {:<16} driver error: {}",
            "lc_ctype=NONE:",
            e.to_string().lines().next().unwrap_or("")
        ),
    }
    let (bin_raw,): (String,) = t2
        .query_first("SELECT name_bin FROM t WHERE name_bin = 'Café'", ())?
        .unwrap();
    hexdump("NONE, name_bin:", &bin_raw);
    t2.commit()?;

    println!(
        "  -> the column stores E9 (WIN1252); the UTF8 connection receives the\n\
         \x20    transliterated C3 A9, the NONE connection the raw stored byte E9 —\n\
         \x20    which rsfbclient's mandatory UTF-8 String decode then rejects (the\n\
         \x20    error above is the passthrough, observed).  The UTF8-column bytes\n\
         \x20    C3 A9 pass through NONE untouched and happen to be valid UTF-8."
    );

    println!("\ndone.");
    Ok(())
}
