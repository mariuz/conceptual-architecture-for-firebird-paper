//! lock_manager.rs — the lock-manager scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/lock_manager.cpp.  rsfbclient builds the
//! TPB from a typed TransactionConfiguration and cannot express
//! `RESERVING` (no custom TPB), so — like the JavaScript twin — this
//! sample drives the same three lck_wait modes through ROW conflicts:
//! waiting on a locked record means waiting on the blocking transaction's
//! LCK_tra lock, the same enqueue / wait_for_request paths, reached
//! through MVCC.  The mapping onto the TPB is direct:
//!
//!     TrLockResolution::NoWait        -> isc_tpb_nowait       (lck_wait == 0)
//!     TrLockResolution::Wait(Some(3)) -> isc_tpb_lock_timeout (lck_wait < 0)
//!     TrLockResolution::Wait(None)    -> isc_tpb_wait         (lck_wait > 0)
//!
//! A final act builds a real deadlock through LCK_tra locks and measures
//! how long the periodic scanner (DeadlockTimeout = 10 s) takes to find
//! it.  See ../../lock-manager.md.
//!
//! Run (see ../README.md):  cargo run --bin lock_manager

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};
use std::time::Instant;

fn cfg(lock: TrLockResolution) -> TransactionConfiguration {
    TransactionConfiguration {
        isolation: TrIsolationLevel::ReadCommited(TrRecordVersion::RecordVersion),
        lock_resolution: lock,
        ..TransactionConfiguration::default()
    }
}

// The whole status vector on one line: the second clause is what tells
// an immediate conflict, a lock timeout and a detected deadlock apart.
fn first_line(e: &FbError) -> String {
    e.to_string()
        .lines()
        .map(|l| l.trim_start_matches('-'))
        .collect::<Vec<_>>()
        .join("; ")
}

fn secs(t0: Instant) -> f64 {
    t0.elapsed().as_secs_f64()
}

// One probe: try to update the contested row under the given lck_wait mode.
fn probe(
    conn: &mut rsfbclient::SimpleConnection,
    label: &str,
    lock: TrLockResolution,
) -> Result<(), FbError> {
    let t0 = Instant::now();
    let mut tr = SimpleTransaction::new(conn, cfg(lock))?;
    match tr.execute("update t1 set v = v + 1 where id = 1", ()) {
        Ok(_) => {
            println!("{:<16} granted after {:.3} s", label, secs(t0));
            tr.commit()?;
        }
        Err(e) => {
            println!("{:<16} failed after {:.3} s: {}", label, secs(t0), first_line(&e));
            tr.rollback()?;
        }
    }
    Ok(())
}

fn main() -> Result<(), FbError> {
    let mut a = connect("lock_manager")?;
    let mut b = connect("lock_manager")?;

    {
        let mut ddl = SimpleTransaction::new(&mut a, Default::default())?;
        let _ = ddl.execute("drop table t1", ());
        ddl.execute("create table t1 (id int primary key, v int)", ())?;
        ddl.commit()?;
    }
    {
        let mut ins = SimpleTransaction::new(&mut a, Default::default())?;
        ins.execute("insert into t1 values (1, 0)", ())?;
        ins.execute("insert into t1 values (2, 0)", ())?;
        ins.commit()?;
    }

    println!(
        "note: rsfbclient has no RESERVING / custom TPB — probing lck_wait through\n\
         row conflicts (the blocking transaction's LCK_tra lock), like the JS twin.\n"
    );

    // The holder's uncommitted update makes row 1 contested: every probe
    // below ends up waiting on the holder transaction's LCK_tra lock.
    let mut hold = SimpleTransaction::new(&mut a, cfg(TrLockResolution::Wait(None)))?;
    hold.execute("update t1 set v = 100 where id = 1", ())?;
    println!("holder: row 1 updated, uncommitted (LCK_tra held)");

    probe(&mut b, "NO WAIT:", TrLockResolution::NoWait)?;
    probe(&mut b, "LOCK TIMEOUT 3:", TrLockResolution::Wait(Some(3)))?;

    // WAIT parks in wait_for_request until the holder lets go: the probe
    // runs on its own connection in a thread while we commit 2 s later.
    let waiter = std::thread::spawn(|| -> Result<(), FbError> {
        let mut c = connect("lock_manager")?;
        probe(&mut c, "WAIT:", TrLockResolution::Wait(None))
    });
    std::thread::sleep(std::time::Duration::from_secs(2));
    hold.commit()?;
    println!("holder: committed (2 s later) -> lock released");
    waiter.join().expect("waiter thread panicked")?;

    // Act two: a genuine wait-for cycle through LCK_tra locks.  Both sides
    // block in WAIT mode; nobody looks for the cycle until the periodic
    // scan fires — expect ~DeadlockTimeout seconds, not ~0.
    println!("building deadlock: A updates row 1, B updates row 2, then cross...");
    let snapshot_wait = TransactionConfiguration {
        isolation: TrIsolationLevel::Concurrency,
        lock_resolution: TrLockResolution::Wait(None),
        ..TransactionConfiguration::default()
    };
    let mut tr_a = SimpleTransaction::new(&mut a, snapshot_wait)?;
    tr_a.execute("update t1 set v = v + 1 where id = 1", ())?;

    let (ready_tx, ready_rx) = std::sync::mpsc::channel::<()>();
    let t0 = Instant::now();
    let cross_b = std::thread::spawn(move || -> Result<(), FbError> {
        let mut c = connect("lock_manager")?;
        let mut tr_b = SimpleTransaction::new(&mut c, snapshot_wait)?;
        tr_b.execute("update t1 set v = v + 1 where id = 2", ())?;
        let _ = ready_tx.send(());
        match tr_b.execute("update t1 set v = v + 1 where id = 1", ()) {
            Ok(_) => println!(
                "deadlock: B's update proceeded after {:.1} s (A was the victim)",
                secs(t0)
            ),
            Err(e) => println!("deadlock: B failed after {:.1} s: {}", secs(t0), first_line(&e)),
        }
        tr_b.rollback()?; // frees A if B was the victim; plain cleanup otherwise
        Ok(())
    });

    ready_rx.recv().expect("cross thread died before updating row 2");
    std::thread::sleep(std::time::Duration::from_millis(300));
    match tr_a.execute("update t1 set v = v + 1 where id = 2", ()) {
        Ok(_) => println!(
            "deadlock: A's update proceeded after {:.1} s (B was the victim)",
            secs(t0)
        ),
        Err(e) => println!("deadlock: A failed after {:.1} s: {}", secs(t0), first_line(&e)),
    }
    tr_a.rollback()?; // frees B if A was the victim
    cross_b.join().expect("cross thread panicked")?;

    println!(
        "the wait is DeadlockTimeout (10 s default): the cycle sat undetected \
         until the scan."
    );
    Ok(())
}
