//! pooling.rs — both directions of pooling, as far as rsfbclient reaches.
//!
//! The rsfbclient twin of ../cpp/pooling.cpp (see ../../connection-pooling.md).
//!
//!   1. OUTBOUND: the server's own external-connections (EDS) pool, driven
//!      and observed purely through SQL — three EXECUTE STATEMENT ON
//!      EXTERNAL calls reuse one outbound connection, watched via the
//!      EXT_CONN_POOL_* context variables (tuning it needs the
//!      MODIFY_EXT_CONN_POOL privilege; SYSDBA has it).
//!   2. INBOUND: what a client-side pool would cache is attachments, so the
//!      lifecycle is shown in MON$ATTACHMENTS: two extra connections appear
//!      as two attachment rows, dropping one issues a real detach and its
//!      row vanishes.  rsfbclient itself ships no pool — the checkout/
//!      checkin layer lives in a separate crate, r2d2_firebird (an r2d2
//!      adapter in the rsfbclient repository), standing in for the inbound
//!      pooler Firebird deliberately does not ship.
//!
//! Run (see ../README.md):  cargo run --bin pooling

use rsfbclient::{prelude::*, FbError, SimpleConnection, SimpleTransaction};

fn attach_employee() -> Result<SimpleConnection, FbError> {
    let conn = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(std::env::args().nth(1).unwrap_or_else(|| "employee".into()))
        .user(fb_handson_rust::user())
        .pass(fb_handson_rust::password())
        .connect()?;
    Ok(conn.into())
}

// The pool's four SYSTEM context variables in one row.
fn pool_state(tr: &mut SimpleTransaction, moment: &str) -> Result<(), FbError> {
    type Vars = (Option<String>, Option<String>, Option<String>, Option<String>);
    let (size, life, idle, active): Vars = tr
        .query_first(
            "select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_SIZE'),\
                    rdb$get_context('SYSTEM', 'EXT_CONN_POOL_LIFETIME'),\
                    rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT'),\
                    rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT')\
             from rdb$database",
            (),
        )?
        .unwrap();
    let v = |o: Option<String>| o.unwrap_or_else(|| "<null>".into());
    println!(
        "{:<18} size={} lifetime={}s idle={} active={}",
        moment,
        v(size),
        v(life),
        v(idle),
        v(active)
    );
    Ok(())
}

fn outbound() -> Result<(), FbError> {
    println!("--- outbound: the server-side EDS pool ---");
    let external = std::env::args().nth(2).unwrap_or_else(|| "inet://localhost/employee".into());
    let mut conn = attach_employee()?;

    // 1. Tune the pool at runtime (per server process, not persistent).
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    tr.execute("alter external connections pool set size 5", ())?;
    tr.execute("alter external connections pool set lifetime 30 second", ())?;
    tr.commit_retaining()?;

    pool_state(&mut tr, "before:")?;

    // 2. Three EXECUTE STATEMENT ON EXTERNAL calls to the SAME
    //    (connection string, user, password, role) — the pool's key.
    let block = format!(
        "execute block returns (idle varchar(10), active varchar(10)) as\n\
           declare i int = 0;\n\
           declare v int;\n\
         begin\n\
           while (i < 3) do\n\
           begin\n\
             execute statement 'select 1 from rdb$database'\n\
               on external '{}'\n\
               as user '{}' password '{}'\n\
               into :v;\n\
             i = i + 1;\n\
           end\n\
           idle   = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');\n\
           active = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');\n\
           suspend;\n\
         end",
        external,
        fb_handson_rust::user(),
        fb_handson_rust::password()
    );
    let (idle, active): (String, String) = tr.query_first(&block, ())?.unwrap();
    println!(
        "{:<18} idle={} active={}   (3 calls, 1 outbound connection)",
        "inside the block:", idle, active
    );

    // 3. Full commit: only now is the external connection truly unused —
    //    it is reset with ALTER SESSION RESET and parked on the idle list.
    //    (COMMIT RETAINING keeps the transaction context alive, and with it
    //    the pooled connection stays ACTIVE — try it.)
    tr.commit()?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    pool_state(&mut tr, "after commit:")?;

    // 4. Evict every idle connection now.
    tr.execute("alter external connections pool clear all", ())?;
    pool_state(&mut tr, "after CLEAR ALL:")?;

    tr.commit()?;
    Ok(())
}

fn inbound() -> Result<(), FbError> {
    println!("--- inbound: attachments are what a client pool caches ---");
    let mut monitor = attach_employee()?;

    // Two extra connections — a hand-rolled "pool" of size 2.
    let mut a = attach_employee()?;
    let mut b = attach_employee()?;
    let (id_a,): (i64,) = a
        .query_first("select current_connection from rdb$database", ())?
        .unwrap();
    let (id_b,): (i64,) = b
        .query_first("select current_connection from rdb$database", ())?
        .unwrap();

    // A fresh transaction per look — each gets a fresh MON$ snapshot.
    let mut tr = SimpleTransaction::new(&mut monitor, TransactionConfiguration::default())?;
    let (n,): (i64,) = tr
        .query_first(
            "select count(*) from mon$attachments where mon$attachment_id in (?, ?)",
            (id_a, id_b),
        )?
        .unwrap();
    println!(
        "opened 2:         attachments {} and {} -> {} rows in MON$ATTACHMENTS",
        id_a, id_b, n
    );
    tr.commit()?;

    // Reuse is the whole point of a pool: the same attachment serves query
    // after query under the same MON$ATTACHMENT_ID.
    let (again,): (i64,) = a
        .query_first("select current_connection from rdb$database", ())?
        .unwrap();
    println!(
        "reused conn a:    CURRENT_CONNECTION still {} (a pool would hand this out again)",
        again
    );

    // Dropping the connection is a REAL op_detach — the row disappears.
    drop(a);
    let mut tr = SimpleTransaction::new(&mut monitor, TransactionConfiguration::default())?;
    let (n,): (i64,) = tr
        .query_first(
            "select count(*) from mon$attachments where mon$attachment_id in (?, ?)",
            (id_a, id_b),
        )?
        .unwrap();
    println!("dropped conn a:   {} row left — detach really detached", n);
    tr.commit()?;
    drop(b);

    println!(
        "rsfbclient has no built-in pool; the r2d2_firebird crate (r2d2 adapter\n\
         in the rsfbclient repo) parks these attachments instead of detaching."
    );
    Ok(())
}

fn main() -> Result<(), FbError> {
    outbound()?;
    inbound()?;
    println!("done.");
    Ok(())
}
