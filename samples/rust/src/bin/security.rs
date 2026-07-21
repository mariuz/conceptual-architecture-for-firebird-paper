//! security.rs — the security layers observed from Rust.
//!
//! The rsfbclient twin of ../cpp/security.cpp.  In one run:
//!   1. WHO AM I    — the attachment's own MON$ATTACHMENTS row: auth plugin
//!                    and wire-crypt plugin, layers 1 and 2 as the server
//!                    recorded them for THIS connection.
//!   2. USERS       — SEC$USERS, the virtual view over the security database.
//!   3. LEAST PRIVILEGE — a temporary user plus a role carrying the
//!                    MONITOR_ANY_ATTACHMENT system privilege; connecting
//!                    with and without the role (builder .role()) changes how
//!                    much of MON$ATTACHMENTS the same user can see.
//!   4. FAILED LOGIN — the error chain a wrong password produces.
//! User management is DEFERRED work performed at commit — so every CREATE /
//! DROP USER here runs in its own SimpleTransaction with a full commit,
//! exactly the discipline the C++ sample documents.
//! See ../../security-architecture.md.
//!
//! Run (see ../README.md):  cargo run --bin security

use fb_handson_rust::{connect, db_path};
use rsfbclient::{charset, prelude::*, FbError, SimpleConnection, SimpleTransaction};

const TMP_USER: &str = "HANDSON_USER";
const TMP_PASS: &str = "Hands0nPw";

/// Run one statement in its own transaction, ignoring any error — needed for
/// idempotent cleanup, because DROP USER reports "record not found" only at
/// COMMIT (user management is deferred work, executed at commit time).
fn exec_ignore(conn: &mut SimpleConnection, sql: &str) {
    if let Ok(mut t) = SimpleTransaction::new(conn, TransactionConfiguration::default()) {
        if t.execute(sql, ()).is_ok() {
            let _ = t.commit();
        }
    }
}

fn exec_commit(conn: &mut SimpleConnection, sqls: &[&str]) -> Result<(), FbError> {
    let mut t = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    for sql in sqls {
        t.execute(sql, ())?;
    }
    t.commit() // <- the deferred user-management work happens HERE
}

fn who_am_i(tr: &mut SimpleTransaction, label: &str) -> Result<(), FbError> {
    let (who, auth, wire, proto, role): (
        String,
        Option<String>,
        Option<String>,
        Option<String>,
        String,
    ) = tr
        .query_first(
            "SELECT TRIM(MON$USER), MON$AUTH_METHOD, MON$WIRE_CRYPT_PLUGIN, \
                    MON$REMOTE_PROTOCOL, TRIM(COALESCE(CURRENT_ROLE, 'NONE')) \
             FROM MON$ATTACHMENTS WHERE MON$ATTACHMENT_ID = CURRENT_CONNECTION",
            (),
        )?
        .unwrap();
    let s = |o: Option<String>| o.unwrap_or_else(|| "(none)".into());
    println!(
        "{:<22} user={} auth={} wirecrypt={} protocol={} role={}",
        label, who, s(auth), s(wire), s(proto), role
    );
    Ok(())
}

fn visible_attachments(tr: &mut SimpleTransaction) -> Result<i64, FbError> {
    let (n,): (i64,) = tr
        .query_first(
            "SELECT COUNT(*) FROM MON$ATTACHMENTS WHERE MON$SYSTEM_FLAG = 0",
            (),
        )?
        .unwrap();
    Ok(n)
}

/// Attach to the security scratch database as an arbitrary user, optionally
/// under a role (the builder's .role() is the DPB isc_dpb_sql_role_name).
fn attach_as(u: &str, p: &str, role: Option<&str>) -> Result<SimpleConnection, FbError> {
    let mut b = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(db_path("security"))
        .user(u)
        .pass(p)
        .charset(charset::UTF_8)
        .clone();
    if let Some(r) = role {
        b.role(r);
    }
    Ok(b.connect()?.into())
}

fn main() -> Result<(), FbError> {
    let mut admin = connect("security")?;

    // 1. Layers 1+2, as recorded for THIS attachment.
    {
        let mut t = SimpleTransaction::new(&mut admin, TransactionConfiguration::default())?;
        who_am_i(&mut t, "admin attachment:")?;
        t.commit()?;
    }

    // 2+3. A temporary user (security database) and a privileged role (this
    //      database).  Cleanup first, in case a prior run died.
    exec_ignore(&mut admin, "DROP USER HANDSON_USER USING PLUGIN Srp");
    exec_ignore(&mut admin, "DROP ROLE HANDSON_MONITOR");
    exec_commit(
        &mut admin,
        &["CREATE USER HANDSON_USER PASSWORD 'Hands0nPw' USING PLUGIN Srp"],
    )?;
    exec_commit(
        &mut admin,
        &[
            "CREATE ROLE HANDSON_MONITOR SET SYSTEM PRIVILEGES TO MONITOR_ANY_ATTACHMENT",
            "GRANT HANDSON_MONITOR TO USER HANDSON_USER",
        ],
    )?;

    let mut tra = SimpleTransaction::new(&mut admin, TransactionConfiguration::default())?;
    println!("\nSEC$USERS (the security database, through the virtual view):");
    let users: Vec<(String, String, bool)> = tra.query(
        "SELECT TRIM(SEC$USER_NAME), TRIM(SEC$PLUGIN), SEC$ADMIN FROM SEC$USERS ORDER BY 1",
        (),
    )?;
    println!("    {:<16} {:<8} {}", "USER", "PLUGIN", "ADMIN");
    for (u, p, a) in users {
        println!("    {:<16} {:<8} {}", u, p, a);
    }

    // 3. Same user, without and with the role: the system privilege decides
    //    how much of MON$ is visible.  (admin stays attached, so a
    //    fully-privileged viewer sees at least 2 attachments.)
    println!(
        "\nadmin sees {} user attachments in MON$ATTACHMENTS",
        visible_attachments(&mut tra)?
    );
    tra.commit()?;

    {
        let mut plain = attach_as(TMP_USER, TMP_PASS, None)?;
        let mut t2 = SimpleTransaction::new(&mut plain, TransactionConfiguration::default())?;
        who_am_i(&mut t2, "user, no role:")?;
        println!(
            "  -> sees {} attachment(s): only its own",
            visible_attachments(&mut t2)?
        );
        t2.commit()?;
    }
    {
        let mut monitor = attach_as(TMP_USER, TMP_PASS, Some("HANDSON_MONITOR"))?;
        let mut t3 = SimpleTransaction::new(&mut monitor, TransactionConfiguration::default())?;
        who_am_i(&mut t3, "user + role:")?;
        println!(
            "  -> sees {} attachments: MONITOR_ANY_ATTACHMENT at work",
            visible_attachments(&mut t3)?
        );
        t3.commit()?;
    }

    // 4. The failed login, and its exact error chain.
    println!("\nfailed login (wrong password) produces:");
    match attach_as(TMP_USER, "wrong-password", None) {
        Ok(_) => println!("    BUG: the wrong password was accepted"),
        Err(e) => println!("    {}", e),
    }

    // Cleanup, again one transaction per DDL batch with a full commit.
    exec_commit(
        &mut admin,
        &[
            "DROP USER HANDSON_USER USING PLUGIN Srp",
            "DROP ROLE HANDSON_MONITOR",
        ],
    )?;
    println!("\ntemporary user and role dropped. done.");
    Ok(())
}
