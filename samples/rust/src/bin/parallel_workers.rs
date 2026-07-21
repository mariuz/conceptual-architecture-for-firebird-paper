//! parallel_workers.rs — watching worker attachments appear (and not).
//!
//! The rsfbclient twin of ../cpp/parallel_workers.cpp (see
//! ../../parallel-workers.md).  Two phases:
//!
//! [A] Against the live server: the C++ twin asks for 4 workers via
//!     isc_dpb_parallel_workers and collects the isc_bad_par_workers
//!     WARNING; rsfbclient's builder exposes no such DPB knob, so this
//!     phase shows what any client can still see — both knobs are GLOBAL
//!     config, readable through RDB$CONFIG, and this attachment's grant
//!     sits in MON$PARALLEL_WORKERS.  With the stock MaxParallelWorkers=1
//!     no request could be honored anyway.
//!
//! [B] Against an embedded engine whose private FIREBIRD root sets
//!     ParallelWorkers = 4 / MaxParallelWorkers = 8 (with_embedded() runs
//!     the whole engine in-process, so its firebird.conf is ours to write):
//!     build a wide 200k-row table, CREATE INDEX on it, and poll
//!     MON$ATTACHMENTS from a second attachment while the build runs.  The
//!     workers appear as ordinary attachments — MON$USER = '<Worker>',
//!     MON$SYSTEM_FLAG = 1 — and stay pooled (idle timeout 60 s) after the
//!     build: parallelism built out of attachments, exactly as the
//!     document argues.
//!
//! Run (see ../README.md):  cargo run --bin parallel_workers

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleConnection, SimpleTransaction};
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::{Arc, Mutex};

const ROOT: &str = "/tmp/fbhandson_rust_emb/fbroot-parallel";
const EMB_DB: &str = "/tmp/fbhandson_rust_emb/parallel_rust.fdb";

// A private $FIREBIRD root: symlinks into the stock install, own firebird.conf.
fn make_root() -> Result<(), FbError> {
    std::fs::create_dir_all(ROOT)?;
    for f in [
        "plugins",
        "intl",
        "firebird.msg",
        "tzdata",
        "plugins.conf",
        "databases.conf",
    ] {
        let _ = std::os::unix::fs::symlink(
            format!("/opt/firebird/{}", f),
            format!("{}/{}", ROOT, f),
        );
    }
    std::fs::write(
        format!("{}/firebird.conf", ROOT),
        "ServerMode = Super\nParallelWorkers = 4\nMaxParallelWorkers = 8\n",
    )?;
    Ok(())
}

fn attach_embedded() -> Result<SimpleConnection, FbError> {
    let mut b = rsfbclient::builder_native().with_dyn_link().with_embedded();
    b.db_name(EMB_DB).user(fb_handson_rust::user());
    match b.connect() {
        Ok(c) => Ok(c.into()),
        Err(_) => Ok(b.create_database()?.into()),
    }
}

fn knobs(tr: &mut SimpleTransaction) -> Result<String, FbError> {
    let one = |tr: &mut SimpleTransaction, name: &str| -> Result<String, FbError> {
        let row: Option<(String,)> = tr.query_first(
            "select rdb$config_value from rdb$config where rdb$config_name = ?",
            (name,),
        )?;
        Ok(row.map(|(v,)| v).unwrap_or_else(|| "?".into()))
    };
    Ok(format!(
        "ParallelWorkers = {}, MaxParallelWorkers = {}",
        one(tr, "ParallelWorkers")?,
        one(tr, "MaxParallelWorkers")?
    ))
}

fn main() -> Result<(), FbError> {
    // Read by the embedded engine of phase B when it boots in-process.
    std::env::set_var("FIREBIRD", ROOT);
    make_root()?;

    // --- [A] the live server: the knobs every client can read ------------
    {
        let mut conn = connect("parallel_workers")?;
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        println!("[A] server attach (rsfbclient has no isc_dpb_parallel_workers knob)");
        println!("    server config: {}", knobs(&mut tr)?);
        let (mine,): (i64,) = tr
            .query_first(
                "select mon$parallel_workers from mon$attachments \
                 where mon$attachment_id = current_connection",
                (),
            )?
            .unwrap();
        println!(
            "    this attachment's MON$PARALLEL_WORKERS = {} -> no request could \
             be honored anyway\n",
            mine
        );
        tr.commit()?;
    }

    // --- [B] embedded engine with its own firebird.conf ------------------
    let mut conn = attach_embedded()?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    println!("[B] embedded attach, FIREBIRD={}", ROOT);
    println!("    engine config: {}", knobs(&mut tr)?);

    let _ = tr.execute("drop table parade", ());
    tr.commit_retaining()?;
    tr.execute("create table parade (id int, val varchar(200))", ())?;
    tr.commit_retaining()?;
    // The filler must be incompressible: records are RLE-compressed on
    // page, and IndexCreateTask::getMaxWorkers() goes parallel only if
    // the relation spans more than one pointer page.
    tr.execute(
        "execute block as declare n int = 0; begin \
           while (n < 200000) do begin \
             insert into parade values (:n, \
               uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || \
               uuid_to_char(gen_uuid()) || uuid_to_char(gen_uuid()) || \
               uuid_to_char(gen_uuid())); \
             n = n + 1; \
           end end",
        (),
    )?;
    tr.commit_retaining()?;
    let (pp,): (i64,) = tr
        .query_first(
            "select count(*) from rdb$pages p join rdb$relations r \
               on p.rdb$relation_id = r.rdb$relation_id \
             where r.rdb$relation_name = 'PARADE' and p.rdb$page_type = 4",
            (),
        )?
        .unwrap();
    println!(
        "    parade table: 200000 rows of 180 incompressible bytes, {} pointer pages",
        pp
    );

    // A second attachment (same process, same in-process engine) polls
    // MON$ATTACHMENTS while the index build runs.
    let stop = Arc::new(AtomicBool::new(false));
    let max_seen = Arc::new(AtomicI64::new(0));
    let roster = Arc::new(Mutex::new(String::new()));
    let poller = {
        let (stop, max_seen, roster) = (stop.clone(), max_seen.clone(), roster.clone());
        std::thread::spawn(move || {
            let poll = || -> Result<(), FbError> {
                let mut mon = attach_embedded()?;
                while !stop.load(Ordering::Relaxed) {
                    // a fresh transaction gets a fresh MON$ snapshot
                    let mut t =
                        SimpleTransaction::new(&mut mon, TransactionConfiguration::default())?;
                    let (n,): (i64,) = t
                        .query_first(
                            "select count(*) from mon$attachments \
                             where mon$user = '<Worker>'",
                            (),
                        )?
                        .unwrap();
                    if n > max_seen.load(Ordering::Relaxed) {
                        max_seen.store(n, Ordering::Relaxed);
                        let rows: Vec<(String, i64)> = t.query(
                            "select trim(mon$user), mon$system_flag \
                             from mon$attachments order by mon$attachment_id",
                            (),
                        )?;
                        let mut r = String::new();
                        for (user, flag) in rows {
                            r += &format!("        {}  (system_flag {})\n", user, flag);
                        }
                        *roster.lock().unwrap() = r;
                    }
                    t.commit()?;
                    std::thread::sleep(std::time::Duration::from_millis(20));
                }
                Ok(())
            };
            let _ = poll(); // like the C++ twin: a racing MON$ poll may just stop
        })
    };

    let t0 = std::time::Instant::now();
    tr.execute("create index ix_parade on parade (val)", ())?;
    tr.commit_retaining()?;
    let ms = t0.elapsed().as_millis();
    stop.store(true, Ordering::Relaxed);
    poller.join().ok();

    println!(
        "    create index: {} ms; max '<Worker>' attachments seen: {}",
        ms,
        max_seen.load(Ordering::Relaxed)
    );
    println!(
        "    MON$ATTACHMENTS at the widest moment:\n{}",
        roster.lock().unwrap()
    );
    let (pooled,): (i64,) = tr
        .query_first(
            "select count(*) from mon$attachments where mon$user = '<Worker>'",
            (),
        )?
        .unwrap();
    println!(
        "    after build: workers stay pooled (idle timeout 60 s): {}",
        pooled
    );
    tr.commit()?;
    println!("done.");
    Ok(())
}
