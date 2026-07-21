//! backup.rs — companion to ../../backup-and-recovery.md
//!
//! The rsfbclient twin of ../cpp/backup.cpp.  The C++ sample drives a gbak
//! backup + restore round trip through the Services API (isc_action_svc_backup
//! / _restore, streaming the verbose log line by line), and node-firebird's
//! pure-JS twin does the same over op_service_attach.  rsfbclient reaches
//! NEITHER: it has no service-manager attach at all — no backup, restore,
//! trace or nbackup actions.  That gap is the honest delta here.
//!
//! What a Rust program can still do is what any driver-less client does:
//! prepare the source database in-driver, then spawn the gbak tool with
//! `-se service_mgr` — the exact same server-side service path the C++
//! sample speaks natively — and verify the restored copy in-driver again.
//! The backup runs while the source attachment is still open: gbak reads
//! through a snapshot transaction (the "online" property of the document's
//! gbak section).
//!
//! Run (see ../README.md):  cargo run --bin backup

use fb_handson_rust::{connect, db_path, password, user};
use rsfbclient::{prelude::*, FbError, SimpleTransaction};
use std::process::Command;

const FBK: &str = "/tmp/fbhandson/backup_rust.fbk"; // server-side path
const DB_RESTORED: &str = "/tmp/fbhandson/backup_rust_restored.fdb";

/// Run one gbak action against the server's service manager and print its
/// verbose log, prefixed like the C++ sample's isc_info_svc_line loop.
fn gbak(args: &[&str]) -> Result<(), FbError> {
    let bin = if std::path::Path::new("/opt/firebird/bin/gbak").exists() {
        "/opt/firebird/bin/gbak"
    } else {
        "gbak"
    };
    let out = Command::new(bin)
        .args(["-se", "localhost:service_mgr", "-user", &user(), "-pas", &password(), "-v"])
        .args(args)
        .output()
        .map_err(|e| FbError::from(format!("could not spawn {}: {}", bin, e)))?;
    for line in String::from_utf8_lossy(&out.stdout).lines() {
        println!("  gbak> {}", line);
    }
    if !out.status.success() {
        return Err(FbError::from(format!(
            "gbak failed: {}",
            String::from_utf8_lossy(&out.stderr)
        )));
    }
    Ok(())
}

fn main() -> Result<(), FbError> {
    let db_src = db_path("backup");

    // -- 1. scratch source database (idempotent) ----------------------------
    let mut conn = connect("backup")?;
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let _ = tr.execute("DROP TABLE BR_ITEMS", ());
        tr.execute(
            "CREATE TABLE BR_ITEMS (ID INT NOT NULL PRIMARY KEY, NAME VARCHAR(30))",
            (),
        )?;
        tr.commit()?;
    }
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute("INSERT INTO BR_ITEMS VALUES (1, 'alpha')", ())?;
        tr.execute("INSERT INTO BR_ITEMS VALUES (2, 'beta')", ())?;
        tr.execute("INSERT INTO BR_ITEMS VALUES (3, 'gamma')", ())?;
        tr.commit()?;
    }
    println!("source ready: BR_ITEMS with 3 rows");

    // -- 2. the honest delta ------------------------------------------------
    println!();
    println!("rsfbclient has no Services API: nothing in the crate can attach to");
    println!("service_mgr, so isc_action_svc_backup/_restore are out of reach from");
    println!("Rust code.  The service path itself still exists, of course — below");
    println!("it is driven by spawning gbak with -se service_mgr, which sends the");
    println!("same SPB action blocks backup.cpp builds in-process.");

    // -- 3. backup while the source attachment is still open ----------------
    println!("\n== backup: {} -> {} ==", db_src, FBK);
    gbak(&["-b", &db_src, FBK])?;

    // -- 4. restore (replace) -----------------------------------------------
    println!("\n== restore: {} -> {} ==", FBK, DB_RESTORED);
    gbak(&["-c", "-rep", FBK, DB_RESTORED])?;

    // -- 5. prove the restored copy has the data, back in-driver ------------
    let mut restored = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(DB_RESTORED)
        .user(user())
        .pass(password())
        .connect()?;
    let (n, mx): (i64, String) = restored
        .query_first("SELECT COUNT(*), MAX(NAME) FROM BR_ITEMS", ())?
        .unwrap();
    println!("\nrestored database says: {} rows, max name = {}", n, mx);
    println!("done.");
    Ok(())
}
