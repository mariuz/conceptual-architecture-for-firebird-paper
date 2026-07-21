//! protocol.rs — one server, two wire-protocol implementations.
//!
//! The rsfbclient twin of ../../protocol_client.cpp (and of nodejs/query.js):
//! attach to the same server twice and let the ENGINE report what was
//! negotiated on each wire.  The native backend loads libfbclient — the very
//! library the C++ samples link — so its attachment shows fbclient's Remote
//! provider at work (Srp256, ChaCha64, the newest protocol version).  The
//! pure-Rust backend is rsfbclient's OWN implementation of the wire protocol
//! (rsfbclient-rust: op_connect/op_attach XDR packets, protocol versions
//! 10–13, SRP client-proof auth with Srp = SHA-1 and Srp256 = SHA-256, and
//! Arc4 wire encryption) — no fbclient anywhere in that call path, just like
//! node-firebird's JavaScript handshake.  MON$ATTACHMENTS records the
//! difference: same server, same user, but an older protocol version and a
//! different wire cipher.
//! See ../../firebird-wire-protocol.md.
//!
//! Run (see ../README.md):  cargo run --bin protocol

use fb_handson_rust::{connect, db_path, password, user};
use rsfbclient::{charset, prelude::*, FbError, SimpleConnection, SimpleTransaction};

/// Ask the ENGINE (not the driver) what this attachment negotiated.
fn report(conn: &mut SimpleConnection, label: &str) -> Result<(), FbError> {
    let mut tr = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    type MonRow = (
        Option<String>, // engine version
        String,         // authenticated user
        Option<String>, // auth plugin
        Option<String>, // wire protocol version, e.g. P19 / P13
        Option<String>, // wire crypt plugin
        Option<String>, // network protocol
        Option<String>, // client library version string, if the client sent one
    );
    let row: Option<MonRow> = tr.query_first(
        "SELECT RDB$GET_CONTEXT('SYSTEM', 'ENGINE_VERSION'), TRIM(CURRENT_USER), \
                MON$AUTH_METHOD, MON$REMOTE_VERSION, MON$WIRE_CRYPT_PLUGIN, \
                MON$REMOTE_PROTOCOL, MON$CLIENT_VERSION \
         FROM MON$ATTACHMENTS WHERE MON$ATTACHMENT_ID = CURRENT_CONNECTION",
        (),
    )?;
    let (engine, who, auth, proto, crypt, net, client) =
        row.ok_or_else(|| FbError::from("no MON$ATTACHMENTS row".to_string()))?;
    let s = |o: Option<String>| o.unwrap_or_else(|| "(none)".into());

    println!("-- {}", label);
    println!("   engine version : {}", s(engine));
    println!("   authenticated  : {}", who);
    println!("   auth method    : {}", s(auth));
    println!("   wire protocol  : {}", s(proto));
    println!("   wire crypt     : {}", s(crypt));
    println!("   network        : {}", s(net));
    println!("   client version : {}", s(client));
    tr.commit()?;
    Ok(())
}

fn main() -> Result<(), FbError> {
    // 1. Native backend: rsfbclient -> libfbclient -> Y-valve -> Remote
    //    provider -> TCP.  The same handshake protocol_client.cpp triggers.
    let mut native = connect("protocol")?; // creates the scratch db if needed
    report(
        &mut native,
        "native backend (libfbclient — the same client library the C++ samples use)",
    )?;
    native.close()?;

    // 2. Pure-Rust backend: rsfbclient-rust opens the TCP socket itself and
    //    speaks XDR-encoded protocol packets — an independent implementation
    //    of the wire protocol, like node-firebird's, with no fbclient below.
    let mut b = rsfbclient::builder_pure_rust();
    b.host("localhost")
        .db_name(db_path("protocol"))
        .user(user())
        .pass(password())
        .charset(charset::UTF_8);
    let mut pure: SimpleConnection = b.connect()?.into();
    report(
        &mut pure,
        "pure_rust backend (rsfbclient-rust — the wire protocol re-implemented in Rust)",
    )?;
    pure.close()?;

    println!();
    println!("rsfbclient-rust implements protocol versions 10-13, Srp (SHA-1) and");
    println!("Srp256 (SHA-256) authentication, and Arc4 wire encryption — so the");
    println!("server meets each client where it stands: Srp256 satisfies both, but");
    println!("the wire drops to protocol 13 and Arc4 where fbclient negotiated the");
    println!("newest version and ChaCha64.");
    println!("done.");
    Ok(())
}
