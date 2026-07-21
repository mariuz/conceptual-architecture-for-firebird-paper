//! embedded_demo.rs — the full server engine, loaded into this process.
//!
//! The rsfbclient twin of ../cpp/embedded_demo.cpp: the native backend
//! (with_dyn_link) links the same libfbclient the C++ samples use, so the
//! whole embedded continuum is reachable from Rust — unlike the pure-JS
//! twin (../nodejs/embedded-demo.js), whose only transport is the socket.
//! Three demonstrations:
//!
//!   1. libfbclient is client AND engine: before the first with_embedded()
//!      attach only libfbclient.so is mapped into the process; the attach
//!      makes the Y-valve load the Engine provider (plugins/libEngine14.so),
//!      and /proc/self/maps proves it.
//!   2. Real work, no server: CREATE TABLE / INSERT / SELECT against a
//!      local .fdb owned by this process (NETWORK_PROTOCOL is NULL and
//!      MON$SERVER_PID is our own pid).
//!   3. The continuum is measurable: attach/detach timed embedded vs
//!      remote — same API, same engine; the difference is the socket, the
//!      SRP handshake and a server round-trip per call.
//!
//! The embedded database must live in a directory THIS process owns (the
//! server must not touch it); default below, override with argv[1].
//!
//! Run (see ../README.md):  cargo run --bin embedded_demo

use rsfbclient::{charset, prelude::*, FbError, SimpleConnection, SimpleTransaction};
use std::time::Instant;

/// Is a shared object whose name contains `frag` mapped into this process?
fn mapped(frag: &str) -> bool {
    std::fs::read_to_string("/proc/self/maps")
        .map(|m| m.contains(frag))
        .unwrap_or(false)
}

/// In-process attach: no host, no port, no credentials that matter —
/// the engine trusts the OS user it is running as.
fn attach_embedded(path: &str) -> Result<SimpleConnection, FbError> {
    let b = rsfbclient::builder_native()
        .with_dyn_link()
        .with_embedded()
        .db_name(path)
        .user(fb_handson_rust::user())
        .charset(charset::UTF_8)
        .clone();
    match b.connect() {
        Ok(c) => Ok(c.into()),
        Err(_) => Ok(b.create_database()?.into()),
    }
}

fn attach_remote(db: &str) -> Result<SimpleConnection, FbError> {
    Ok(rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(db)
        .user(fb_handson_rust::user())
        .pass(fb_handson_rust::password())
        .charset(charset::UTF_8)
        .clone()
        .connect()?
        .into())
}

fn attach_ms(f: &dyn Fn() -> Result<SimpleConnection, FbError>) -> Result<f64, FbError> {
    let t0 = Instant::now();
    drop(f()?); // detach happens in Drop
    Ok(t0.elapsed().as_secs_f64() * 1000.0)
}

fn main() -> Result<(), FbError> {
    let local_db = std::env::args().nth(1).unwrap_or_else(|| {
        let dir = std::env::temp_dir().join("fb_handson_embedded");
        let _ = std::fs::create_dir_all(&dir);
        dir.join("embedded_demo_rust.fdb").to_string_lossy().into_owned()
    });
    let remote_db = std::env::args().nth(2).unwrap_or_else(|| "employee".into());

    // --- 1. watch the engine arrive in our address space -----------------
    println!("before attach: libfbclient mapped={}, libEngine14 mapped={}",
        if mapped("libfbclient") { "yes" } else { "no" },
        if mapped("libEngine14") { "yes" } else { "no" });

    let mut conn = attach_embedded(&local_db)?;

    println!("after  attach: libfbclient mapped={}, libEngine14 mapped={}\n",
        if mapped("libfbclient") { "yes" } else { "no" },
        if mapped("libEngine14") { "yes" } else { "no" });

    // --- 2. real work with no server anywhere ----------------------------
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let _ = tr.execute("drop table gadgets", ());
        tr.execute("create table gadgets (id int primary key, name varchar(20))", ())?;
        tr.commit_retaining()?;
        tr.execute("insert into gadgets values (1, 'sprocket')", ())?;
        tr.execute("insert into gadgets values (2, 'flange')", ())?;
        tr.execute("insert into gadgets values (3, 'grommet')", ())?;
        tr.commit_retaining()?;

        let (rows, maxname, protocol, engine_pid): (i64, String, String, i64) = tr
            .query_first(
                "select count(*), max(name), \
                        coalesce(rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), \
                                 '<null: in-process>'), \
                        a.mon$server_pid \
                 from gadgets, mon$attachments a \
                 where a.mon$attachment_id = current_connection \
                 group by 3, 4",
                (),
            )?
            .unwrap();
        println!("rows={}  max(name)={}  NETWORK_PROTOCOL={}", rows, maxname, protocol);
        println!("engine pid={}, my pid={} — the 'server' is this process\n",
            engine_pid, std::process::id());
        tr.commit()?;
    }

    // --- 3. attach cost: in-process call vs socket + SRP handshake -------
    let embedded = {
        let p = local_db.clone();
        move || attach_embedded(&p)
    };
    let remote = {
        let d = remote_db.clone();
        move || attach_remote(&d)
    };
    const RUNS: usize = 5;
    attach_ms(&embedded)?; // warm-up (provider already loaded anyway)
    attach_ms(&remote)?;   // warm-up (socket/auth code paths)
    let (mut emb, mut rem) = (0.0, 0.0);
    for _ in 0..RUNS {
        emb += attach_ms(&embedded)?;
        rem += attach_ms(&remote)?;
    }
    println!("attach+detach avg over {} runs:", RUNS);
    println!("    embedded  {:<44} {:>7.2} ms", local_db, emb / RUNS as f64);
    println!("    remote    {:<44} {:>7.2} ms", remote_db, rem / RUNS as f64);
    println!("done.");
    Ok(())
}
