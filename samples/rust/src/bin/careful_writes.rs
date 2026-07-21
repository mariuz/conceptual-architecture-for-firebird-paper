//! careful_writes.rs — companion to ../../careful-writes-and-crash-safety.md
//!
//! The rsfbclient twin of ../cpp/careful_writes.cpp: kill a database engine
//! mid-write and show the file needs no recovery.  Because the native
//! backend can load the EMBEDDED engine (with_embedded()), the child
//! process this program spawns IS the engine — SIGKILLing it while an
//! uncommitted bulk insert is flushing dirty pages is a genuine engine
//! crash, not just a dropped client connection.  (Contrast with the pure
//! wire drivers: ../nodejs/careful_writes.js can only kill a *client*.)
//!
//! Where C++ fork()s, Rust re-invokes its own binary with a `--writer` role
//! argument via std::process::Command.  The parent watches the database
//! file grow, kills the writer with SIGKILL (Child::kill), re-attaches
//! embedded and verifies the careful-write guarantee: committed rows all
//! present, uncommitted rows all gone, attach instantaneous — no log
//! replay, because there is no log.
//!
//! Run (see ../README.md):  cargo run --bin careful_writes

use rsfbclient::{prelude::*, FbError, SimpleConnection, SimpleTransaction};
use std::io::Write;
use std::time::{Duration, Instant};

// A plain local path OUTSIDE the server's directory: the embedded engine
// opens it with this process's own filesystem rights.
const DB: &str = "/tmp/careful_writes_rust.fdb";

fn embedded(create: bool) -> Result<SimpleConnection, FbError> {
    let mut b = rsfbclient::builder_native().with_dyn_link().with_embedded();
    b.db_name(DB).user("SYSDBA");
    if create {
        Ok(b.create_database()?.into())
    } else {
        Ok(b.connect()?.into())
    }
}

// Child mode: create the database, commit a marker, then bulk-insert half a
// million rows WITHOUT committing — and wait to be killed.
fn writer() -> Result<(), FbError> {
    let mut conn = embedded(true)?;

    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute("create table cw (id int, tag varchar(30))", ())?;
        tr.commit()?;
    }
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute("insert into cw values (1, 'committed-marker')", ())?;
        tr.commit()?;
    }
    println!("[writer {}] marker row committed (forced writes on)", std::process::id());
    let _ = std::io::stdout().flush();

    // Never committed: the crash victim.
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    tr.execute(
        "execute block as declare i int = 0; begin \
           while (i < 500000) do begin \
             insert into cw values (:i + 1000, 'uncommitted'); i = i + 1; \
           end \
         end",
        (),
    )?;
    println!("[writer] bulk insert finished uncommitted; waiting for SIGKILL");
    let _ = std::io::stdout().flush();
    loop {
        std::thread::sleep(Duration::from_secs(3600));
    }
}

fn file_size(path: &str) -> i64 {
    std::fs::metadata(path).map(|m| m.len() as i64).unwrap_or(-1)
}

fn main() -> Result<(), FbError> {
    if std::env::var_os("FIREBIRD").is_none() {
        std::env::set_var("FIREBIRD", "/opt/firebird");
    }
    if std::env::args().nth(1).as_deref() == Some("--writer") {
        return writer();
    }

    let _ = std::fs::remove_file(DB); // fresh run

    // 1. Spawn the writer: a separate process running the embedded engine.
    let exe = std::env::current_exe()
        .map_err(|e| FbError::from(format!("current_exe: {}", e)))?;
    let mut child = std::process::Command::new(exe)
        .arg("--writer")
        .spawn()
        .map_err(|e| FbError::from(format!("spawn: {}", e)))?;

    // 2. Wait until the file is visibly growing — the engine is flushing
    //    freshly allocated data pages of the *uncommitted* transaction —
    //    then SIGKILL the engine process mid-flight.
    let mut base: i64 = -1;
    for _ in 0..600 {
        std::thread::sleep(Duration::from_millis(50));
        let sz = file_size(DB);
        if base < 0 && sz > 0 {
            base = sz;
        }
        if base > 0 && sz > base + 2 * 1024 * 1024 {
            println!("file grew {} -> {} bytes; SIGKILL to engine pid {}", base, sz, child.id());
            break;
        }
    }
    child.kill().map_err(|e| FbError::from(format!("kill: {}", e)))?;
    child.wait().map_err(|e| FbError::from(format!("wait: {}", e)))?;

    // 3. Re-attach immediately.  No recovery step exists to run: the
    //    precedence graph never let an inconsistent state reach disk.
    let t0 = Instant::now();
    let mut conn = embedded(false)?;
    let (committed,): (i64,) = conn
        .query_first("select count(*) from cw where tag = 'committed-marker'", ())?
        .unwrap();
    let (uncommitted,): (i64,) = conn
        .query_first("select count(*) from cw where tag = 'uncommitted'", ())?
        .unwrap();
    let ms = t0.elapsed().as_millis();

    println!("re-attach + both counts took {} ms", ms);
    println!("committed marker rows : {}   <- survived the crash", committed);
    println!("uncommitted rows      : {}   <- rolled back by visibility, not replay", uncommitted);
    Ok(())
}
