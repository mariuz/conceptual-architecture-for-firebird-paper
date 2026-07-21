//! architecture_comparison.rs — one driver, two ways into the engine (and a third that only knows the wire).
//!
//! The rsfbclient twin of ../cpp/architecture_comparison.cpp, companion to
//! ../../architecture-comparison.md.  The comparison table's most unusual
//! row is Firebird's: "client-server AND embedded, same engine".  Because
//! rsfbclient's native backend links libfbclient — the whole Y-valve and
//! provider stack — a single Rust binary can prove it too:
//!
//!   1. native + with_remote()   — the Y-valve picks the Remote provider;
//!      SQL travels over TCP to the server process.
//!   2. native + with_embedded() — the Y-valve loads the Engine provider
//!      INTO THIS PROCESS; no server is involved at all.
//!   3. pure Rust backend        — rsfbclient's own wire-protocol code.
//!      Like every wire-only driver (see ../nodejs/architecture-comparison.js)
//!      it gets exactly the client-server half of the architecture: embedded
//!      mode is a property of the native client library, and there is no
//!      native library here to load an engine with.
//!
//! Run (see ../README.md):  cargo run --bin architecture_comparison

use fb_handson_rust::{password, user};
use rsfbclient::{prelude::*, FbError, SimpleConnection};

const SQL: &str = "select rdb$get_context('SYSTEM', 'ENGINE_VERSION'), \
                          rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), \
                          a.mon$server_pid \
                   from mon$attachments a \
                   where a.mon$attachment_id = current_connection";

fn inspect(conn: &mut SimpleConnection, label: &str, conn_str: &str) -> Result<(), FbError> {
    let (version, protocol, server_pid): (String, Option<String>, i64) =
        conn.query_first(SQL, ())?.unwrap();

    let my_pid = std::process::id() as i64;
    println!("{}", label);
    println!("    connection        : {}", conn_str);
    println!("    ENGINE_VERSION    : {}", version);
    println!(
        "    NETWORK_PROTOCOL  : {}",
        protocol.as_deref().unwrap_or("<null>")
    );
    println!(
        "    MON$SERVER_PID    : {}   (this process is pid {}{})",
        server_pid,
        my_pid,
        if server_pid == my_pid {
            " -- the engine runs IN this process"
        } else {
            ""
        }
    );
    Ok(())
}

fn main() -> Result<(), FbError> {
    let remote_db = std::env::args().nth(1).unwrap_or_else(|| "employee".into());
    // A plain local path, NOT under the server's ownership: the embedded
    // engine opens it with this process's own filesystem rights.
    let embedded_db = std::env::args()
        .nth(2)
        .unwrap_or_else(|| "/tmp/arch_embedded_rust.fdb".into());

    if std::env::var_os("FIREBIRD").is_none() {
        std::env::set_var("FIREBIRD", "/opt/firebird");
    }

    println!("One libfbclient behind the native backend, two providers behind its Y-valve.\n");

    // -- 1. Remote provider: classic client-server over TCP ----------------
    let mut remote: SimpleConnection = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(&remote_db)
        .user(user())
        .pass(password())
        .connect()?
        .into();
    inspect(&mut remote, "[1] native + with_remote() (client-server):", &remote_db)?;

    // -- 2. Engine provider: the same libfbclient becomes the engine -------
    let mut builder = rsfbclient::builder_native().with_dyn_link().with_embedded();
    builder.db_name(&embedded_db).user(user());
    let mut embedded: SimpleConnection = match builder.connect() {
        Ok(c) => c.into(),
        Err(_) => builder.create_database()?.into(),
    };
    println!();
    inspect(
        &mut embedded,
        "[2] native + with_embedded() (embedded, no server):",
        &embedded_db,
    )?;

    // -- 3. Pure Rust backend: the wire protocol and nothing else ----------
    let mut pure: SimpleConnection = rsfbclient::builder_pure_rust()
        .host("localhost")
        .db_name(&remote_db)
        .user(user())
        .pass(password())
        .connect()?
        .into();
    println!();
    inspect(&mut pure, "[3] pure Rust backend (wire protocol only):", &remote_db)?;
    println!("    There is no embedded variant of this backend: it reimplements the");
    println!("    Remote protocol in Rust, so a database path is always resolved by");
    println!("    a server process, never by this one.");

    println!("\ndone.");
    Ok(())
}
