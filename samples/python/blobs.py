#
# blobs.py - BLOBs through firebird-driver's two blob paths (Python twin of
# ../cpp/blobs.cpp; see ../../blob-handling.md).
#
# The C++ twin drives IBlob by hand: three putSegment() calls, a getSegment()
# loop that returns the same three boundaries, getInfo() statistics.  This
# driver decides the blob shape from the Python VALUE bound to the parameter:
#   - a str/bytes becomes a SEGMENTED blob, written in 64 KB putSegment()s;
#   - a file-like object (anything with .read()) becomes a STREAM blob, one
#     putSegment() per read() call - so a reader yielding three pieces writes
#     the C++ sample's three segments, into a stream blob.
# On fetch a blob is materialised into str/bytes, or - for columns named in
# cursor.stream_blobs (or longer than stream_blob_threshold) - handed back
# as a BlobReader, a file-like io object with read()/readline()/seek()/tell()
# plus .length and .blob_type.  What the public API does not surface: segment
# boundaries on read (read() coalesces) and the num_segments/max_segment
# info items (the driver asks for them internally to size its reads).
#
# Run:  python3 blobs.py [database]
#
from firebird.driver import DatabaseError

from fbsample import attach_or_create, db_path, error_text, execute, query, run

SEGMENTS = ['first segment', 'second, longer segment', 'third']


class SegmentSource:
    """A minimal file-like object: each read() returns the next segment."""
    def __init__(self, pieces):
        self.pieces = list(pieces)

    def read(self, size=-1):
        return self.pieces.pop(0) if self.pieces else ''


def show(names, rows):
    rows = [['<null>' if v is None else str(v) for v in r] for r in rows]
    widths = [max(len(n), *(len(r[i]) for r in rows)) for i, n in enumerate(names)]
    print(' '.join(n.ljust(w) for n, w in zip(names, widths)).rstrip())
    print(' '.join('-' * w for w in widths))
    for r in rows:
        print(' '.join(v.ljust(w) for v, w in zip(r, widths)).rstrip())


def main():
    with attach_or_create(db_path('blobs')) as con:
        execute(con, 'recreate table docs (id integer primary key,'
                     ' note blob sub_type text character set utf8,'
                     ' data blob sub_type binary)')
        cur = con.cursor()

        # -- 1. two writes, two blob shapes, chosen by the Python value ------
        cur.execute('insert into docs (id, note) values (?, ?)',
                    (1, SegmentSource(SEGMENTS)))
        print('id 1: file-like value, 3 read() calls -> 3 putSegment()s, stream blob')
        cur.execute('insert into docs (id, note) values (?, ?)', (2, ''.join(SEGMENTS)))
        print('id 2: str value -> segmented blob')
        cur.execute('insert into docs (id, data) values (?, ?)', (3, bytes(range(256))))

        # -- 2. read back as BlobReader (file-like) ---------------------------
        cur.stream_blobs.append('NOTE')
        for blob_id in (1, 2):
            cur.execute('select note from docs where id = ?', (blob_id,))
            reader = cur.fetchone()[0]
            print(f'\nid {blob_id}: {type(reader).__name__}, length {reader.length},'
                  f' {reader.blob_type.name} blob, mode {reader.mode!r}')
            print(f'  read(): {reader.read()!r}')
            try:
                reader.seek(35)
                print(f'  seek(35) + read(): {reader.read()!r}')
            except DatabaseError as e:
                print('  seek(35) refused:', error_text(e).replace('\n', ' / '))
            reader.close()
        cur.stream_blobs.clear()

        # Materialised: a binary blob is just bytes.
        data = query(con, 'select data from docs where id = 3')[0][0]
        print(f'\nid 3: {type(data).__name__}, {len(data)} bytes,'
              f' round-trip intact: {data == bytes(range(256))}')

        # -- 3. subtype text vs binary, from the catalog ---------------------
        print('\n-- column subtypes (RDB$FIELDS) --')
        show(['FIELD', 'SUBTYPE', 'CHARSET'], query(con,
             "select trim(rf.rdb$field_name), f.rdb$field_sub_type,"
             " trim(cs.rdb$character_set_name)"
             " from rdb$relation_fields rf"
             " join rdb$fields f on rf.rdb$field_source = f.rdb$field_name"
             " left join rdb$character_sets cs"
             "   on f.rdb$character_set_id = cs.rdb$character_set_id"
             " where rf.rdb$relation_name = 'DOCS' and f.rdb$field_type = 261"
             " order by 1"))

        # -- 4. BLOB_APPEND: build a blob in SQL; a text blob comes back str --
        cur.execute("insert into docs (id, note) values (4, blob_append("
                    "cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))")
        print('\n-- BLOB_APPEND result --')
        show(['ID', 'OCTETS', 'CHARS', 'CONTENT'], query(con,
             'select id, octet_length(note), char_length(note), note'
             ' from docs where id = 4'))
        cur.close()
        con.commit()
    print('done.')


if __name__ == '__main__':
    run(main)
