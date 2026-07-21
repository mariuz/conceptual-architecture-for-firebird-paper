//! stmt_cache.rs — TWO statement caches, told apart by timing.
//!
//! The rsfbclient twin of ../cpp/stmt_cache.cpp — with a twist the C++
//! sample does not have: rsfbclient keeps its own CLIENT-side cache of
//! prepared statements (per connection, LRU, keyed by the verbatim SQL
//! text; `stmt_cache_size(n)` on the builder, default 20).  The document's
//! subject is the SERVER-side DSQL cache, so this sample must first get
//! the client cache out of the way: `stmt_cache_size(0)` makes every
//! query() send a real prepare, and the server cache becomes visible in
//! the prepare cost — exactly as in the C++ timings.  Like the JavaScript
//! twin, rsfbclient cannot prepare without executing, so the probe query
//! (a six-way self-join) is heavy to *compile* and nearly free to run.
//!
//!   0  identical text, client cache ON   -> no prepare is even sent
//!   1  identical text, client cache OFF  -> prepare each time: server hits
//!   2  + i trailing spaces               -> server misses: the key is the
//!                                          text verbatim, spaces included
//!   3  distinct literal each time        -> server misses, for comparison
//!   4  identical text, DDL commit before -> server misses: any committed
//!      each prepare                        DDL purges the whole cache
//! See ../../statement-cache.md.
//!
//! Run (see ../README.md):  cargo run --bin stmt_cache

use fb_handson_rust::{connect, db_path, password, user};
use rsfbclient::{charset, prelude::*, FbError, SimpleConnection, SimpleTransaction};
use std::time::Instant;

const HEAVY: &str = "SELECT COUNT(*) FROM t a \
     JOIN t b ON a.id = b.id JOIN t c ON b.id = c.id \
     JOIN t d ON c.id = d.id JOIN t e ON d.id = e.id \
     JOIN t f ON e.id = f.id WHERE a.id > 0";

const N: usize = 100;

/// A second attachment to the same database, with the client-side
/// statement cache sized as asked (0 = every query really prepares).
fn connect_with_cache(size: usize) -> Result<SimpleConnection, FbError> {
    Ok(rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(db_path("stmt_cache"))
        .user(user())
        .pass(password())
        .charset(charset::UTF_8)
        .stmt_cache_size(size)
        .connect()?
        .into())
}

fn report(label: &str, verdict: &str, ms: f64) {
    println!(
        "{} {:3} queries: {:7.1} ms  ({:.2} ms/query) - {}",
        label,
        N,
        ms,
        ms / N as f64,
        verdict
    );
}

fn main() -> Result<(), FbError> {
    // Helper connection: creates the database if needed, runs the DDL.
    let mut ddl_conn = connect("stmt_cache")?;
    {
        let mut setup = SimpleTransaction::new(&mut ddl_conn, TransactionConfiguration::default())?;
        setup.execute("RECREATE TABLE t (id INT NOT NULL PRIMARY KEY)", ())?;
        setup.commit_retaining()?;
        setup.execute(
            "EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN WHILE (i <= 50) DO \
             BEGIN INSERT INTO t VALUES (:i); i = i + 1; END END",
            (),
        )?;
        setup.commit()?;
    }

    // -- 0. The CLIENT cache, default size 20: the first query prepares,
    //       every repeat reuses the client-held statement — no prepare
    //       crosses the wire at all.  This is rsfbclient's own cache, not
    //       the document's subject; it must be switched off to see that one.
    let mut cached = connect_with_cache(20)?;
    {
        let mut tra = SimpleTransaction::new(&mut cached, TransactionConfiguration::default())?;
        let _: Vec<(i64,)> = tra.query(HEAVY, ())?; // warm both caches
        let t0 = Instant::now();
        for _ in 0..N {
            let _: Vec<(i64,)> = tra.query(HEAVY, ())?;
        }
        report(
            "0. identical text, client cache on ",
            "client hits: no prepare sent",
            t0.elapsed().as_secs_f64() * 1000.0,
        );
        tra.commit()?;
    }

    // The probe connection: client cache off, every query really prepares.
    let mut conn = connect_with_cache(0)?;
    let mut tra = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    let _: Vec<(i64,)> = tra.query(HEAVY, ())?; // warm the SERVER cache

    // -- 1. Identical text, prepare each time: the server cache serves the
    //       compiled statement, so the prepare is cheap.
    let t0 = Instant::now();
    for _ in 0..N {
        let _: Vec<(i64,)> = tra.query(HEAVY, ())?;
    }
    report(
        "1. identical text, client cache off",
        "server hits",
        t0.elapsed().as_secs_f64() * 1000.0,
    );

    // -- 2. The cache key is the SQL text verbatim: i trailing spaces make
    //       a different key, and every prepare compiles from scratch.
    let t0 = Instant::now();
    for i in 0..N {
        let sql = format!("{}{}", HEAVY, " ".repeat(i + 1));
        let _: Vec<(i64,)> = tra.query(&sql, ())?;
    }
    report(
        "2. + i trailing spaces             ",
        "misses",
        t0.elapsed().as_secs_f64() * 1000.0,
    );

    // -- 3. A distinct literal each time — the usual way applications
    //       accidentally defeat the cache (parameters would not).
    let t0 = Instant::now();
    for i in 0..N {
        let sql = HEAVY.replace("> 0", &format!("> {}", i));
        let _: Vec<(i64,)> = tra.query(&sql, ())?;
    }
    report(
        "3. distinct literal                ",
        "misses",
        t0.elapsed().as_secs_f64() * 1000.0,
    );

    // -- 4. Identical text again, but an unrelated DDL commit before each
    //       prepare: every committed DDL purges the whole database's cache.
    let mut total = 0.0;
    for _ in 0..N {
        let mut ddl = SimpleTransaction::new(&mut ddl_conn, TransactionConfiguration::default())?;
        ddl.execute("RECREATE TABLE unrelated (x INT)", ())?;
        ddl.commit()?; // purges every statement cache in the database

        let t0 = Instant::now(); // time only the query, not the DDL
        let _: Vec<(i64,)> = tra.query(HEAVY, ())?;
        total += t0.elapsed().as_secs_f64() * 1000.0;
    }
    report(
        "4. identical text after DDL commit ",
        "misses",
        total,
    );

    tra.commit()?;
    println!("done.");
    Ok(())
}
