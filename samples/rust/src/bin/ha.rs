//! ha.rs — the high-availability scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/ha.cpp: the one HA primitive that is pure
//! client-side SQL, a database SHADOW.  Creates a shadow on the scratch
//! database, proves the mirror file appears (RDB$FILES plus a metadata()
//! of the file — server and sample share this host), shows the shadow
//! growing in lock-step with the main file under write load, and drops it.
//! The other primitives (replica promotion, sync_replica) need server-side
//! config and stay as text in the document.
//! See ../../high-availability.md.
//!
//! Run (see ../README.md):  cargo run --bin ha

use fb_handson_rust::{connect, db_path};
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

const SHADOW: &str = "/tmp/fbhandson/ha_rust.shd";

fn file_size(path: &str) -> i64 {
    std::fs::metadata(path).map(|m| m.len() as i64).unwrap_or(-1)
}

fn show_files(when: &str, main: &str) {
    println!("{:<28} main = {:>8} bytes, shadow = {:>8} bytes",
        when, file_size(main), file_size(SHADOW));
}

fn main() -> Result<(), FbError> {
    let main_file = db_path("ha");
    let mut conn = connect("ha")?;

    // Idempotent cleanup from earlier runs.
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let _ = tr.execute("DROP SHADOW 1 DELETE FILE", ());
        let _ = tr.execute("DROP TABLE HA_LOG", ());
        tr.execute(
            "CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))",
            (),
        )?;
        tr.commit()?;
    }

    // 1. Create the synchronous page-level mirror.
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute(&format!("CREATE SHADOW 1 '{}'", SHADOW), ())?;
        tr.commit()?;
    }
    println!("CREATE SHADOW 1 done — the engine dumped every page to the mirror\n");

    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    // The shadow is registered in the metadata like any other file.
    let files: Vec<(String, Option<i64>, Option<i64>)> = tr.query(
        "SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS \
         FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER",
        (),
    )?;
    for (name, shadow_no, flags) in &files {
        println!("  {:<32} shadow# {:<3} flags {}",
            name.trim(), shadow_no.unwrap_or(0), flags.unwrap_or(0));
    }
    println!();
    show_files("after CREATE SHADOW:", &main_file);

    // 2. Write load: every page write now goes to both files.
    tr.execute(
        "EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN \
           WHILE (I < 5000) DO BEGIN \
             INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1; \
           END \
         END",
        (),
    )?;
    tr.commit_retaining()?;
    show_files("after 5000 inserts:", &main_file);

    // 3. Retire the mirror.
    tr.execute("DROP SHADOW 1 DELETE FILE", ())?;
    tr.commit()?;
    println!("\nDROP SHADOW 1 DELETE FILE done");
    show_files("after DROP SHADOW:", &main_file);

    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    let (left,): (i64,) = tr.query_first("SELECT COUNT(*) FROM RDB$FILES", ())?.unwrap();
    tr.commit()?;
    println!("\nRDB$FILES rows left: {}", left);
    println!("done.");
    Ok(())
}
