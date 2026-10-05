#
# catalog.py - the catalog describing itself, from client SQL (Python twin
# of ../cpp/catalog.cpp; see ../../catalog-bootstrap.md).
#
# On a freshly created database: the fixed relation ids (RDB$PAGES 0,
# RDB$DATABASE 1, RDB$FIELDS 2, RDB$RELATIONS 6); RDB$PAGES carrying its own
# pointer page, cross-checked against the hdr_PAGES word at byte 28 of
# page 0; RDB$FORMATS empty under a full system catalog (formats as code);
# and user DDL planting the first stored formats.  firebird-driver makes the
# fresh-file dance two calls - Connection.drop_database() and
# create_database() - and the anchor check stays as primitive as in every
# other language: open(), seek(28), struct.unpack('<I').  Run it on the
# server machine (it reads the file the server wrote).
#
# Run:  python3 catalog.py [database [local-file]]
#
import struct
import sys

from fbsample import db_path, execute, recreate, run, scalar


def table(con, sql, names=None):
    """isql-style listing; headers from cursor.description unless given."""
    cur = con.cursor()
    cur.execute(sql)
    rows = [['<null>' if v is None else str(v).strip() for v in r] for r in cur.fetchall()]
    names = names or [d[0] for d in cur.description]
    cur.close()
    widths = [max([len(n)] + [len(r[i]) for r in rows]) for i, n in enumerate(names)]
    print(' '.join(n.ljust(w) for n, w in zip(names, widths)).rstrip())
    print(' '.join('-' * w for w in widths))
    for r in rows:
        print(' '.join(v.ljust(w) for v, w in zip(r, widths)).rstrip())


def main():
    database = db_path('catalog')
    local_file = sys.argv[2] if len(sys.argv) > 2 else database

    # A truly fresh database each run: drop it if it exists, recreate.
    with recreate(database) as con:
        print('-- 1. fixed relation ids (relations.h declaration order) --')
        table(con, 'select rdb$relation_id, trim(rdb$relation_name) '
                   'from rdb$relations where rdb$relation_id in (0, 1, 2, 6) order by 1',
              ['ID', 'NAME'])

        print('\n-- 2. RDB$PAGES describing relation 0 (itself) and relation 6 (RDB$RELATIONS) --')
        table(con, 'select rdb$page_number, rdb$relation_id, rdb$page_sequence, rdb$page_type '
                   'from rdb$pages where rdb$relation_id in (0, 6) '
                   'order by rdb$relation_id, rdb$page_type, rdb$page_number')

        # The anchor that cuts the recursion: hdr_PAGES at byte 28 of page 0.
        try:
            with open(local_file, 'rb') as f:
                f.seek(28)
                (hdr_pages,) = struct.unpack('<I', f.read(4))
            print(f'\nhdr_PAGES (page 0, offset 28) = {hdr_pages}'
                  '  <- matches the (relation 0, type 4) row above')
        except OSError as e:
            print(f'\n(cannot read {local_file} for the hdr_PAGES check: {e.strerror})')

        print('\n-- 3. formats as code: zero stored formats, yet a full catalog --')
        table(con, 'select (select count(*) from rdb$formats), '
                   '       (select count(*) from rdb$relations where rdb$system_flag = 1), '
                   '       (select count(*) from rdb$relation_fields r join rdb$relations rel '
                   '          on r.rdb$relation_name = rel.rdb$relation_name '
                   '          and r.rdb$schema_name = rel.rdb$schema_name '
                   '        where rel.rdb$system_flag = 1) '
                   'from rdb$database',
              ['FORMATS_ROWS', 'SYS_RELATIONS', 'SYS_FIELDS'])
        con.commit()

        print('\n-- 4. user DDL writes formats into the catalog --')
        execute(con, 'create table t1 (a integer)')
        execute(con, 'alter table t1 add b varchar(10)')
        table(con, 'select rdb$relation_id, rdb$format, octet_length(rdb$descriptor) '
                   'from rdb$formats order by rdb$relation_id, rdb$format',
              ['RDB$RELATION_ID', 'RDB$FORMAT', 'DESCRIPTOR_BYTES'])
        rel_id = scalar(con, "select rdb$relation_id from rdb$relations "
                             "where rdb$relation_name = 'T1'")
        print(f'\n(relation id of T1: {rel_id} - the first user id; '
              'system tables still contribute no rows)')
        con.commit()
    print('done.')


if __name__ == '__main__':
    run(main)
