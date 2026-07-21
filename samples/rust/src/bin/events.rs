//! events.rs — the POST_EVENT notification scenario in Rust.
//!
//! The rsfbclient twin of ../events_demo.cpp: a listener and a poster on
//! the same database, proving the two commit semantics of Firebird events
//! (a ROLLBACK swallows posts; delivery happens at COMMIT, not when
//! POST_EVENT executes).  Events need the NATIVE backend: rsfbclient
//! exposes the blocking isc_wait_for_event dance as
//! SimpleConnection::wait_for_event(name) and wraps it in a handler loop
//! as listen_event(name, closure); the pure_rust backend implements no
//! auxiliary event channel and returns an error — an honest gap this
//! sample demonstrates at the end.  Another honest delta: wait_for_event
//! returns only the wakeup, so the "one delivery, count 3" that the C++
//! twin reads with isc_event_counts stays inside the client library here.
//!
//! Run (see ../README.md):  cargo run --bin events

use fb_handson_rust::{connect, db_path, password, user};
use rsfbclient::{
    charset, prelude::*, FbError, RemoteEventsManager, SimpleConnection, SimpleTransaction,
};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

const POST: &str = "EXECUTE BLOCK AS BEGIN POST_EVENT 'demo_event'; END";

fn sleep_ms(ms: u64) {
    thread::sleep(Duration::from_millis(ms));
}

fn main() -> Result<(), FbError> {
    let listener = connect("events")?; // creates the scratch db if missing
    let mut poster = connect("events")?;

    // --- 1. a blocked wait_for_event, and what does NOT wake it ----------
    // wait_for_event registers interest (consuming the baseline delivery
    // internally) and blocks, so it runs on its own thread; the main
    // thread watches whether the thread has finished.
    let waiter = thread::spawn(move || {
        let mut c = listener;
        let r = c.wait_for_event("demo_event".to_string());
        (c, r)
    });
    sleep_ms(1000); // let the interest get registered
    println!("listener blocked in wait_for_event('demo_event')");

    // POST_EVENT then ROLLBACK: nothing may be delivered.
    let mut tr = SimpleTransaction::new(&mut poster, TransactionConfiguration::default())?;
    tr.execute(POST, ())?;
    tr.rollback()?;
    sleep_ms(1500);
    println!("after POST_EVENT + ROLLBACK: listener {}",
        if waiter.is_finished() { "woke (UNEXPECTED)" }
        else { "still blocked  (correct - rollback swallows posts)" });

    // Three POST_EVENTs in one transaction: delivery only at COMMIT.
    let mut tr = SimpleTransaction::new(&mut poster, TransactionConfiguration::default())?;
    tr.execute(POST, ())?;
    tr.execute(POST, ())?;
    tr.execute(POST, ())?;
    println!("3 x POST_EVENT executed, not yet committed - waiting briefly...");
    sleep_ms(1200);
    println!("before COMMIT: listener {}",
        if waiter.is_finished() { "woke (UNEXPECTED)" }
        else { "still blocked  (correct - delivery is commit-time)" });

    tr.commit()?;
    let mut waited = 0;
    while !waiter.is_finished() && waited < 5000 {
        sleep_ms(50);
        waited += 50;
    }
    if !waiter.is_finished() {
        return Err(FbError::from("event was never delivered after COMMIT"));
    }
    let (listener, wait_result) = waiter.join().expect("waiter thread panicked");
    wait_result?;
    println!("after COMMIT: wait_for_event returned  (correct - posts delivered at commit)");
    println!("  (the C++ twin reads 'one delivery, count 3' via isc_event_counts;");
    println!("   rsfbclient reports only the wakeup - the count stays in the client)");

    // --- 2. listen_event: the same one-shot re-queue dance, packaged -----
    // The closure runs on a listener thread after every delivery (the
    // library re-registers in between, because interests are one-shot);
    // returning false stops the loop.  listen_event on a SimpleConnection
    // can't infer which backend it wraps, so this part uses a typed native
    // Connection, on which the trait resolves unambiguously.
    drop(listener);
    let typed_listener = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(db_path("events"))
        .user(user())
        .pass(password())
        .charset(charset::UTF_8)
        .clone()
        .connect()?;
    let fired = Arc::new(Mutex::new(0));
    let fired_in_handler = Arc::clone(&fired);
    let handler = typed_listener.listen_event("demo_event".to_string(), move |c| {
        // the handler gets the connection back: prove it can run SQL
        let (one,): (i64,) = c.query_first("select 1 from rdb$database", ())?.unwrap();
        let mut n = fired_in_handler.lock().unwrap();
        *n += one;
        Ok(*n < 2) // stop the loop after the second delivery
    })?;
    sleep_ms(1000);

    let mut posts = 0;
    while !handler.is_finished() && posts < 6 {
        let mut tr = SimpleTransaction::new(&mut poster, TransactionConfiguration::default())?;
        tr.execute(POST, ())?;
        tr.commit()?;
        posts += 1;
        sleep_ms(1200);
    }
    if !handler.is_finished() {
        return Err(FbError::from("listen_event handler never reached 2 deliveries"));
    }
    handler.join().expect("listener thread panicked")?;
    println!("\nlisten_event: handler fired {} times over {} committed posts,",
        *fired.lock().unwrap(), posts);
    println!("  then returned false and the library stopped the re-queue loop");

    // --- 3. the pure_rust backend has no event channel --------------------
    let mut pure: SimpleConnection = rsfbclient::builder_pure_rust()
        .host("localhost")
        .db_name(db_path("events"))
        .user(user())
        .pass(password())
        .connect()?
        .into();
    match pure.wait_for_event("demo_event".to_string()) {
        Ok(()) => println!("\npure_rust wait_for_event: returned (UNEXPECTED)"),
        Err(e) => println!("\npure_rust backend, same call: \"{}\"", e),
    }
    println!("  (the wire implementation in rsfbclient-rust has no aux channel yet)");

    println!("done.");
    Ok(())
}
