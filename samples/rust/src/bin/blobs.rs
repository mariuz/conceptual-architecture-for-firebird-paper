//! blobs.rs — companion to ../../blob-handling.md
//!
//! The rsfbclient twin of ../cpp/blobs.cpp.  The C++ sample works at the
//! segment level: createBlob/putSegment/getSegment and a getInfo call that
//! asks the blob for its own statistics.  rsfbclient exposes NONE of that —
//! a blob is simply a whole Rust value.  What it does inside is still the
//! segmented API: a String parameter longer than 32767 bytes, and every
//! Vec<u8> parameter, is written with isc_create_blob + isc_put_segment in
//! 64K chunks; on fetch the driver drains isc_get_segment into one buffer
//! (255 bytes at a time) and hands back a String or Vec<u8>.  Segment
//! boundaries, blob info, seek and the segmented/stream distinction are all
//! invisible — those deltas are part of the lesson.  The catalog's view of
//! subtypes and SQL's BLOB_APPEND work exactly as in C++.
//!
//! Run (see ../README.md):  cargo run --bin blobs

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn main() -> Result<(), FbError> {
    let mut conn = connect("blobs")?;

    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute(
            "recreate table docs (\
             id integer primary key,\
             note blob sub_type text character set utf8,\
             data blob sub_type binary)",
            (),
        )?;
        tr.commit()?;
    }

    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    // -- 1. a text blob as a plain String value -------------------------------
    // Short text params travel as VARCHAR; the SERVER coerces them into the
    // blob column.  The driver never touches the blob API here.
    let short = "first segment / second, longer segment / third";
    tr.execute("insert into docs (id, note) values (1, ?)", (short,))?;
    let (back,): (String,) = tr
        .query_first("select note from docs where id = 1", ())?
        .unwrap();
    println!("text blob round trip (short String param, sent as VARCHAR):");
    println!("  wrote {} chars, read back {} chars: \"{}\"", short.len(), back.len(), back);

    // -- 2. a long String param: now the driver itself creates a blob --------
    // Above MAX_TEXT_LENGTH (32767) rsfbclient switches to isc_create_blob and
    // writes isc_put_segment chunks of up to 65535 bytes — the segmented API
    // the C++ sample calls by hand, hidden behind one parameter bind.
    let long = "0123456789".repeat(4000); // 40000 bytes > 32767
    tr.execute("insert into docs (id, note) values (2, ?)", (long.as_str(),))?;
    let (octets, chars): (i64, i64) = tr
        .query_first("select octet_length(note), char_length(note) from docs where id = 2", ())?
        .unwrap();
    println!("\nlong text param ({} bytes > 32767): driver-created blob, {} octets / {} chars on the server", long.len(), octets, chars);

    // -- 3. a binary blob as a Vec<u8> ----------------------------------------
    // Vec<u8> params ALWAYS go through the blob API, whatever their size.
    let bytes: Vec<u8> = (0..1000u32).map(|i| (i % 256) as u8).collect();
    tr.execute("insert into docs (id, data) values (3, ?)", (bytes.clone(),))?;
    let (got,): (Vec<u8>,) = tr
        .query_first("select data from docs where id = 3", ())?
        .unwrap();
    println!(
        "\nbinary blob round trip: wrote {} bytes, read {} bytes, identical: {}",
        bytes.len(),
        got.len(),
        got == bytes
    );

    // -- 4. what the C++ sample can see that this driver cannot ---------------
    println!("\nwhat stays invisible without a blob handle API:");
    println!("  - putSegment/getSegment boundaries: the driver concatenates every");
    println!("    segment into one value (reading 255 bytes per isc_get_segment call)");
    println!("  - getInfo statistics: segment count, longest segment, segmented vs stream");
    println!("  - blob seek, and BPB options like filters or stream blobs");

    // -- 5. subtype text vs binary, from the catalog ---------------------------
    println!("\n-- column subtypes (RDB$FIELDS) --");
    println!("  {:<6} {:>8}  {}", "FIELD", "SUBTYPE", "CHARSET");
    let subs: Vec<(String, i64, Option<String>)> = tr.query(
        "select trim(rf.rdb$field_name), f.rdb$field_sub_type, trim(cs.rdb$character_set_name) \
         from rdb$relation_fields rf \
         join rdb$fields f on rf.rdb$field_source = f.rdb$field_name \
         left join rdb$character_sets cs \
           on f.rdb$character_set_id = cs.rdb$character_set_id \
         where rf.rdb$relation_name = 'DOCS' and f.rdb$field_type = 261 \
         order by 1",
        (),
    )?;
    for (field, subtype, charset) in subs {
        println!(
            "  {:<6} {:>8}  {}",
            field,
            subtype,
            charset.as_deref().unwrap_or("<null>")
        );
    }

    // -- 6. BLOB_APPEND: build a blob in SQL without recopying -----------------
    tr.execute(
        "insert into docs (id, note) values (4, \
         blob_append(cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))",
        (),
    )?;
    println!("\n-- BLOB_APPEND result --");
    let (id, octets, chars, content): (i64, i64, i64, String) = tr
        .query_first(
            "select id, octet_length(note), char_length(note), cast(note as varchar(50)) \
             from docs where id = 4",
            (),
        )?
        .unwrap();
    println!("  ID {}: {} octets, {} chars, content \"{}\"", id, octets, chars, content);

    tr.commit()?;
    println!("\ndone.");
    Ok(())
}
