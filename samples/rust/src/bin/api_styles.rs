//! api_styles.rs — the same query through both backends of rsfbclient.
//!
//! The rsfbclient twin of ../cpp/api_styles.cpp, companion to
//! ../../client-apis-and-drivers.md.  The C++ sample contrasts the two C
//! APIs of libfbclient — the legacy ISC calls with their hand-built DPB and
//! XSQLDA bookkeeping, and the OO interfaces.  rsfbclient's version of that
//! fork in the road is its two BACKENDS behind one Rust API: the NATIVE
//! backend links the very same libfbclient and drives its isc_* entry
//! points (the C++ sample's legacy half, with the descriptor bookkeeping
//! written once inside the driver), while the PURE RUST backend is an
//! independent reimplementation of the wire protocol — no client library,
//! no Y-valve, just sockets.  Same query, same server, two client stacks.
//!
//! Run (see ../README.md):  cargo run --bin api_styles [db]

use fb_handson_rust::{password, user};
use rsfbclient::{prelude::*, FbError, SystemInfos};

const SQL: &str = "select rdb$get_context('SYSTEM', 'ENGINE_VERSION') from rdb$database";

fn main() -> Result<(), FbError> {
    let database = std::env::args().nth(1).unwrap_or_else(|| "employee".into());

    // -- 1. native backend: the same libfbclient the C++ sample links ------
    let mut native = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(&database)
        .user(user())
        .pass(password())
        .connect()?;
    let (ver,): (String,) = native.query_first(SQL, ())?.unwrap();
    println!("[native   ] engine version = {}   (isc_* calls inside libfbclient)", ver);

    // -- 2. pure Rust backend: rsfbclient's own wire-protocol code ---------
    let mut pure = rsfbclient::builder_pure_rust()
        .host("localhost")
        .db_name(&database)
        .user(user())
        .pass(password())
        .connect()?;
    let (ver,): (String,) = pure.query_first(SQL, ())?.unwrap();
    println!("[pure rust] engine version = {}   (op_attach/op_execute on a socket)", ver);

    // -- 3. what the driver adds on top: the SystemInfos convenience trait --
    // db_name() asks the server for DB_NAME — both backends resolve the same
    // server-side file, whatever stack carried the question there.
    println!("\nSystemInfos, one trait over both backends:");
    println!("    native    db_name() = {}", native.db_name()?);
    println!("    pure rust db_name() = {}", pure.db_name()?);

    // server_engine() parses ENGINE_VERSION into an enum that stops at V5 —
    // against Firebird 6 it errors out.  An honest driver-lag delta: the raw
    // query above works fine; only the typed convenience wrapper is behind.
    match native.server_engine() {
        Ok(v) => println!("    server_engine() = {:?}", v),
        Err(e) => println!(
            "    server_engine() = error \"{}\"\n      <- the driver's EngineVersion enum ends at V5; Firebird 6 postdates rsfbclient 0.27",
            e
        ),
    }

    println!("\nsame engine, same server, two client stacks. done.");
    Ok(())
}
