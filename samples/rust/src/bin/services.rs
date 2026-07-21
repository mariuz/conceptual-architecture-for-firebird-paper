//! services.rs — the Services API gap, and its SQL-reachable neighbours.
//!
//! The C++ twin (../cpp/services.cpp) attaches to service_mgr, starts a real
//! gbak backup on a server thread and drains the 1 KB svc_stdout ring buffer;
//! node-firebird (../nodejs/services.js) does the same over its own wire
//! implementation.  rsfbclient has NO Services API at all: no
//! attachServiceManager, no SPB builder, no isc_info_svc_line loop — so this
//! twin shows what a plain SQL attachment CAN reach of the same territory:
//!
//!   1. the information requests (server version, page size, ODS, sweep
//!      interval) answered from RDB$GET_CONTEXT and MON$DATABASE;
//!   2. user management — a Services action in the gsec era — as SQL DDL;
//!   3. nbackup's lock/unlock as ALTER DATABASE BEGIN/END BACKUP, watching
//!      MON$BACKUP_STATE flip;
//!
//! and states honestly what stays out of reach: gbak backup/restore, trace
//! sessions, validation and sweep still need service_mgr (or the command-line
//! tools).  See ../../services-api.md.
//!
//! Run (see ../README.md):  cargo run --bin services

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleConnection, SimpleTransaction, SystemInfos};

fn backup_state(conn: &mut SimpleConnection) -> Result<String, FbError> {
    // Fresh transaction each time: MON$ snapshots are frozen per transaction.
    let mut tr = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    let (state,): (String,) = tr
        .query_first(
            "SELECT DECODE(MON$BACKUP_STATE, 0, '0 (normal)', 1, '1 (stalled)', \
                    2, '2 (merge)') FROM MON$DATABASE",
            (),
        )?
        .unwrap();
    tr.commit()?;
    Ok(state)
}

fn exec_commit(conn: &mut SimpleConnection, sql: &str) -> Result<(), FbError> {
    let mut tr = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    tr.execute(sql, ())?;
    tr.commit()
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("services")?;

    println!("rsfbclient has no Services API: no service_mgr attach, no SPB, no");
    println!("ring-buffer polling.  What follows is the service territory a plain");
    println!("SQL attachment can still cover.\n");

    // -- 1. The information requests, answered from SQL. -------------------
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let (version, page_size, ods_major, ods_minor, sweep): (String, i64, i64, i64, i64) = tr
            .query_first(
                "SELECT RDB$GET_CONTEXT('SYSTEM', 'ENGINE_VERSION'), \
                        MON$PAGE_SIZE, MON$ODS_MAJOR, MON$ODS_MINOR, \
                        MON$SWEEP_INTERVAL \
                 FROM MON$DATABASE",
                (),
            )?
            .unwrap();
        println!("-- server facts (isc_info_svc_* territory, via SQL)");
        println!("   engine version : {}", version);
        println!("   page size      : {}", page_size);
        println!("   ODS            : {}.{}", ods_major, ods_minor);
        println!("   sweep interval : {}", sweep);
        tr.commit()?;
    }
    // rsfbclient's own SystemInfos::server_engine() parses that same context
    // variable — but its EngineVersion enum stops at V5, so on Firebird 6:
    match conn.server_engine() {
        Ok(v) => println!("   conn.server_engine() -> {:?}", v),
        Err(e) => println!("   conn.server_engine() -> Err: {}   <- enum ends at V5", e),
    }

    // -- 2. User management: once a Services action (gsec), now SQL DDL. ----
    println!("\n-- user management (action_add_user / action_delete_user, via SQL DDL)");
    // Idempotent: a previous crashed run may have left the user behind.
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let _ = tr.execute("DROP USER SVC_DEMO_USER USING PLUGIN Srp", ());
        let _ = tr.commit(); // DROP USER errors surface at commit; ignore
    }
    exec_commit(&mut conn, "CREATE USER SVC_DEMO_USER PASSWORD 'Svc9Demo' USING PLUGIN Srp")?;
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let (n,): (i64,) = tr
            .query_first(
                "SELECT COUNT(*) FROM SEC$USERS WHERE SEC$USER_NAME = 'SVC_DEMO_USER'",
                (),
            )?
            .unwrap();
        println!("   CREATE USER SVC_DEMO_USER -> SEC$USERS rows: {}", n);
        tr.commit()?;
    }
    exec_commit(&mut conn, "DROP USER SVC_DEMO_USER USING PLUGIN Srp")?;
    println!("   DROP USER SVC_DEMO_USER   -> gone again");

    // -- 3. nbackup's lock/unlock, as plain DDL. ---------------------------
    // ALTER DATABASE BEGIN BACKUP freezes the main file (changes go to a
    // .delta) — the SQL face of nbackup -L; END BACKUP merges the delta back.
    println!("\n-- physical-backup lock (nbackup -L/-N territory, via ALTER DATABASE)");
    // Idempotent: leave no stalled state behind from a crashed run.
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let _ = tr.execute("ALTER DATABASE END BACKUP", ());
        let _ = tr.commit();
    }
    println!("   MON$BACKUP_STATE          : {}", backup_state(&mut conn)?);
    exec_commit(&mut conn, "ALTER DATABASE BEGIN BACKUP")?;
    println!("   after BEGIN BACKUP        : {}   <- writes now go to the .delta", backup_state(&mut conn)?);
    exec_commit(&mut conn, "ALTER DATABASE END BACKUP")?;
    println!("   after END BACKUP          : {}   <- delta merged back", backup_state(&mut conn)?);

    // -- 4. What stays behind service_mgr. ---------------------------------
    println!("\nstill Services-only from this driver: gbak backup/restore, trace");
    println!("sessions, online validation, sweep — see ../cpp/services.cpp for the");
    println!("real ring-buffer loop and ../nodejs/services.js for a driver that");
    println!("implements op_service_attach itself.");
    println!("done.");
    Ok(())
}
