//! ods_header.rs — the on-disk structure, read straight from the file.
//!
//! The rsfbclient twin of ../cpp/ods_header.cpp (see ../../on-disk-structure.md).
//! The driver is only needed for the first half: create a scratch database
//! through the server, generate a little transaction history, and ask
//! MON$DATABASE for the server's own view of the numbers.  Then the
//! attachment is closed and std::fs takes over — the header page (page 0)
//! is decoded at the byte offsets src/jrd/ods.h pins with static_asserts,
//! and a census of byte 0 of every page shows "one file, many pages".
//! Run it on the machine the server runs on (the server writes the file;
//! we read it).
//!
//! Run (see ../README.md):  cargo run --bin ods_header

use fb_handson_rust::{connect, db_path};
use rsfbclient::{prelude::*, FbError, SimpleTransaction};
use std::fs;

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

fn u64le(b: &[u8], o: usize) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&b[o..o + 8]);
    u64::from_le_bytes(v)
}

fn page_type_name(t: u8) -> &'static str {
    match t {
        1 => "pag_header",
        2 => "pag_pages (PIP)",
        3 => "pag_transactions (TIP)",
        4 => "pag_pointer",
        5 => "pag_data",
        6 => "pag_root",
        7 => "pag_index (b-tree)",
        8 => "pag_blob",
        9 => "pag_ids (generators)",
        10 => "pag_scns",
        _ => "???",
    }
}

fn main() -> Result<(), FbError> {
    // 1. Create (or reuse) the scratch database and generate a little
    //    transaction history so the TIP markers move off their floor.
    let mut conn = connect("ods_header")?;
    for _ in 0..3 {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let _one: Option<(i64,)> = tr.query_first("select 1 from rdb$database", ())?;
        tr.commit()?;
    }

    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    let (ps, omaj, omin, oit, oat, ost, nxt): (i64, i64, i64, i64, i64, i64, i64) = tr
        .query_first(
            "select mon$page_size, mon$ods_major, mon$ods_minor,\
             mon$oldest_transaction, mon$oldest_active,\
             mon$oldest_snapshot, mon$next_transaction from mon$database",
            (),
        )?
        .unwrap();
    println!("-- server's view (MON$DATABASE) --");
    println!(
        "page size {}, ODS {}.{}, OIT {}, OAT {}, OST {}, next {}",
        ps, omaj, omin, oit, oat, ost, nxt
    );
    tr.commit()?;
    conn.close()?; // detach — the server flushes; now the bytes speak for themselves

    // 2. The same facts straight from the bytes on disk.
    //    struct header_page is 152 bytes.
    let local_file = db_path("ods_header");
    let file = fs::read(&local_file)?;
    let h = &file[..152];

    println!(
        "\n-- header page, parsed from {} (offsets per ods.h) --",
        local_file
    );
    println!("pag_type      @0   = {} ({})", h[0], page_type_name(h[0]));
    println!("pag_flags     @1   = {}", h[1]);

    let page_size = u16le(h, 16) as usize;
    let ods_raw = u16le(h, 18);
    println!("hdr_page_size @16  = {}", page_size);
    println!(
        "hdr_ods_version @18 = 0x{:04x} -> ODS {} (FIREBIRD flag 0x8000 {}), minor @20 = {}",
        ods_raw,
        ods_raw & 0x7fff,
        if ods_raw & 0x8000 != 0 { "set" } else { "clear" },
        u16le(h, 20)
    );

    let flags = u16le(h, 22);
    println!(
        "hdr_flags     @22  = 0x{:02x} ({}{}{})",
        flags,
        if flags & 0x2 != 0 { "force_write " } else { "" },
        if flags & 0x8 != 0 { "no_reserve " } else { "" },
        if flags & 0x10 != 0 { "SQL_dialect_3" } else { "" }
    );
    println!(
        "hdr_PAGES     @28  = {}   <- pointer page of RDB$PAGES (catalog bootstrap anchor)",
        u32le(h, 28)
    );
    println!("hdr_next_transaction   @40 = {}", u64le(h, 40));
    println!("hdr_oldest_transaction @48 = {} (OIT)", u64le(h, 48));
    println!("hdr_oldest_active      @56 = {} (OAT)", u64le(h, 56));
    println!("hdr_oldest_snapshot    @64 = {} (OST)", u64le(h, 64));

    let g = &h[84..100]; // hdr_guid: Win32 GUID layout
    println!(
        "hdr_guid      @84  = {{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
        u32le(g, 0),
        u16le(g, 4),
        u16le(g, 6),
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]
    );

    // 3. Page-type census: byte 0 of every page in the file.
    let mut counts = [0u32; 11];
    let mut pages = 0u64;
    let mut off = 0usize;
    while off < file.len() {
        let t = file[off];
        counts[if t <= 10 { t as usize } else { 0 }] += 1;
        pages += 1;
        off += page_size;
    }

    println!(
        "\n-- page-type census: {} pages of {} bytes --",
        pages, page_size
    );
    for t in 1u8..=10 {
        if counts[t as usize] != 0 {
            println!(
                "  type {:2}  {:<22} {:5}",
                t,
                page_type_name(t),
                counts[t as usize]
            );
        }
    }
    println!("done.");
    Ok(())
}
