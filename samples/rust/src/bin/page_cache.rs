//! page_cache.rs — one shared cache vs. private caches over one file.
//!
//! The rsfbclient twin of ../cpp/page_cache.cpp (see
//! ../../page-cache-coherency.md).  The same hot-page ping-pong runs twice
//! and MON$IO_STATS tells the two cache topologies apart:
//!
//!   phase 1 — two client processes -> ONE SuperServer shared cache
//!             (coherency by shared memory; almost no physical I/O)
//!   phase 2 — two EMBEDDED engine processes with PRIVATE caches over one
//!             file (a ServerMode=SuperClassic sandbox reached through
//!             with_embedded(); coherency by LCK_bdb page locks + blocking
//!             ASTs — data travels through the disk, so the same workload
//!             turns into hundreds of reads AND writes)
//!
//! The parent only spawns re-invocations of itself; every attachment lives
//! in a child process (each embedded child IS a full engine).  Rows 1 and 2
//! share a data page, so the two writers fight over one page without ever
//! touching one row.
//!
//! Run (see ../README.md):  cargo run --bin page_cache

use rsfbclient::{prelude::*, FbError, SimpleConnection, SimpleTransaction};
use std::process::{Child, Command};

const SRV_DB: &str = "/tmp/fbhandson/page_cache_rust.fdb"; // server-side path
const EMB_DIR: &str = "/tmp/fbhandson_rust_emb"; // ours, not the server's
const EMB_DB: &str = "/tmp/fbhandson_rust_emb/page_cache_rust.fdb";
const SANDBOX: &str = "/tmp/fbhandson_rust_emb/fbroot-superclassic";
const ROUNDS: usize = 300;

// "srv" attaches through the server; "emb" runs a whole engine in-process
// (the child was spawned with FIREBIRD pointing at the SuperClassic sandbox).
fn attach(mode: &str, create: bool) -> Result<SimpleConnection, FbError> {
    if mode == "srv" {
        let b = rsfbclient::builder_native()
            .with_dyn_link()
            .with_remote()
            .host("localhost")
            .db_name(SRV_DB)
            .user(fb_handson_rust::user())
            .pass(fb_handson_rust::password())
            .clone();
        match b.connect() {
            Ok(c) => Ok(c.into()),
            Err(e) if create => match b.create_database() {
                Ok(c) => Ok(c.into()),
                Err(_) => Err(e),
            },
            Err(e) => Err(e),
        }
    } else {
        let mut b = rsfbclient::builder_native().with_dyn_link().with_embedded();
        b.db_name(EMB_DB).user(fb_handson_rust::user());
        match b.connect() {
            Ok(c) => Ok(c.into()),
            Err(e) if create => match b.create_database() {
                Ok(c) => Ok(c.into()),
                Err(_) => Err(e),
            },
            Err(e) => Err(e),
        }
    }
}

fn tr_config() -> TransactionConfiguration {
    TransactionConfiguration {
        isolation: TrIsolationLevel::ReadCommited(TrRecordVersion::RecordVersion),
        lock_resolution: TrLockResolution::Wait(None),
        ..TransactionConfiguration::default()
    }
}

fn init_db(mode: &str) -> Result<(), FbError> {
    // --init: fresh table, two rows (they will share one data page)
    let mut conn = attach(mode, true)?;
    let mut tr = SimpleTransaction::new(&mut conn, tr_config())?;
    let _ = tr.execute("drop table t", ());
    tr.commit_retaining()?;
    tr.execute("create table t (id int primary key, v int)", ())?;
    tr.commit_retaining()?;
    tr.execute("insert into t values (1, 0)", ())?;
    tr.execute("insert into t values (2, 0)", ())?;
    tr.commit()?;
    Ok(())
}

fn worker(mode: &str, row_id: &str) -> Result<(), FbError> {
    let mut conn = attach(mode, false)?;
    for _ in 0..ROUNDS {
        let mut tr = SimpleTransaction::new(&mut conn, tr_config())?;
        tr.execute(
            &format!("update t set v = v + 1 where id = {}", row_id),
            (),
        )?;
        tr.commit()?;
    }
    let mut tr = SimpleTransaction::new(&mut conn, tr_config())?;
    let (fetches, reads, writes): (i64, i64, i64) = tr
        .query_first(
            "select MON$PAGE_FETCHES, MON$PAGE_READS, MON$PAGE_WRITES \
             from MON$IO_STATS join MON$ATTACHMENTS using (MON$STAT_ID) \
             where MON$ATTACHMENT_ID = CURRENT_CONNECTION",
            (),
        )?
        .unwrap();
    println!(
        "  worker pid {:<6} row {}: {} commits | page fetches={:<6} reads={:<4} writes={}",
        std::process::id(),
        row_id,
        ROUNDS,
        fetches,
        reads,
        writes
    );
    tr.commit()?;
    Ok(())
}

fn check(mode: &str) -> Result<(), FbError> {
    // --check: any lost updates?
    let mut conn = attach(mode, false)?;
    let mut tr = SimpleTransaction::new(&mut conn, tr_config())?;
    let rows: Vec<(i64, i64)> = tr.query("select id, v from t order by id", ())?;
    for (id, v) in rows {
        println!("  final: id={} v={} (expected {})", id, v, ROUNDS);
    }
    tr.commit()?;
    Ok(())
}

fn spawn_child(args: &[&str], firebird_root: Option<&str>) -> Result<Child, FbError> {
    let mut cmd = Command::new(std::env::current_exe()?);
    cmd.args(args);
    if let Some(root) = firebird_root {
        cmd.env("FIREBIRD", root);
    }
    Ok(cmd.spawn()?)
}

fn run_phase(mode: &str, firebird_root: Option<&str>) -> Result<(), FbError> {
    spawn_child(&["--init", mode], firebird_root)?.wait()?;
    let mut w1 = spawn_child(&["--worker", mode, "1"], firebird_root)?;
    let mut w2 = spawn_child(&["--worker", mode, "2"], firebird_root)?;
    w1.wait()?;
    w2.wait()?;
    spawn_child(&["--check", mode], firebird_root)?.wait()?;
    Ok(())
}

// Build the embedded sandbox: a FIREBIRD root whose firebird.conf says
// SuperClassic, so each process locks the file SHARED and runs its own
// page cache (see 'Three layers of arbitration').
fn make_sandbox() -> Result<(), FbError> {
    std::fs::create_dir_all(SANDBOX)?;
    for f in ["plugins", "intl", "tzdata", "firebird.msg", "security6.fdb"] {
        let _ = std::os::unix::fs::symlink(
            format!("/opt/firebird/{}", f),
            format!("{}/{}", SANDBOX, f),
        );
    }
    std::fs::write(
        format!("{}/firebird.conf", SANDBOX),
        "ServerMode = SuperClassic\n",
    )?;
    let _ = std::fs::remove_file(EMB_DB);
    Ok(())
}

fn main() -> Result<(), FbError> {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("--init") => return init_db(&args[2]),
        Some("--worker") => return worker(&args[2], &args[3]),
        Some("--check") => return check(&args[2]),
        _ => {}
    }

    println!("phase 1: two client processes, ONE SuperServer shared cache");
    run_phase("srv", None)?;

    std::fs::create_dir_all(EMB_DIR)?;
    make_sandbox()?;

    println!("phase 2: two EMBEDDED engine processes, PRIVATE page caches");
    run_phase("emb", Some(SANDBOX))?;
    println!("same workload — the private caches paid for coherency in disk I/O.");
    Ok(())
}
