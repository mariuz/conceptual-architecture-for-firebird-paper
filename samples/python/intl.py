#
# intl.py - charset, collation and transliteration on one small table
# (Python twin of ../cpp/intl.cpp; see ../../internationalization.md).
#
#   1. Collation decides equality: the rows ('Café','CAFE','cafe') match a
#      query for 'cafe' 3 times under UNICODE_CI_AI and once under
#      UCS_BASIC; UPPER() folds accented letters correctly.
#   2. Per-column charsets: one column is UTF8, its neighbour WIN1252.
#   3. Transliteration is driven by the CONNECTION charset (lc_ctype): the
#      same WIN1252 'Café' read over charset='UTF8' and charset='NONE'.
#
# firebird-driver's `charset=` is the lc_ctype AND the codec it decodes
# text with (CHARSET_MAP: 'UTF8' -> utf_8; 'NONE' -> the locale's preferred
# encoding).  So under NONE the untransliterated WIN1252 byte E9 reaches a
# UTF-8 decode and fails - the Rust twin's outcome - but Python's
# UnicodeDecodeError carries the undecodable wire bytes in its .object, so
# the sample still hex-dumps exactly what arrived, without private API.
#
# Run:  python3 intl.py [database]
#
import locale

from firebird.driver import DatabaseError

from fbsample import attach, attach_or_create, db_path, execute, query, run, scalar

SELECT_WIN = "SELECT name_win FROM t WHERE name_bin = 'Café'"


def hexdump(label, raw, shown):
    print(f'  {label:<16} len={len(raw):2}  {raw.hex(" ").upper()}   {shown}')


def fetch_bytes(con):
    """The first value of SELECT_WIN as the bytes the wire carried."""
    try:
        text = scalar(con, SELECT_WIN)
        return text.encode('utf-8'), repr(text)   # UTF8 connection: wire bytes
    except UnicodeDecodeError as e:      # .object = the raw undecodable value
        con.rollback()
        return bytes(e.object), f'UnicodeDecodeError: {e.reason} ({e.encoding})'


def main():
    path = db_path('intl')
    with attach_or_create(path) as db:          # charset UTF8 (fbsample default)
        try:
            execute(db, 'DROP TABLE t')
        except DatabaseError:
            db.rollback()
        execute(db, 'CREATE TABLE t ('
                    '  name_ci_ai VARCHAR(30) CHARACTER SET UTF8 COLLATE UNICODE_CI_AI,'
                    '  name_bin   VARCHAR(30) CHARACTER SET UTF8 COLLATE UCS_BASIC,'
                    '  name_win   VARCHAR(30) CHARACTER SET WIN1252)')
        for v in ('Café', 'CAFE', 'cafe'):
            execute(db, 'INSERT INTO t VALUES (?, ?, ?)', (v, v, v), commit=False)
        db.commit()

        # -- 1. The collation, not the data, decides what "equal" means.
        print("rows matching 'cafe' with UNICODE_CI_AI :",
              scalar(db, "SELECT COUNT(*) FROM t WHERE name_ci_ai = 'cafe'"))
        print("rows matching 'cafe' with UCS_BASIC     :",
              scalar(db, "SELECT COUNT(*) FROM t WHERE name_bin = 'cafe'"))
        print("UPPER('café èñ ß')                      :",
              scalar(db, "SELECT UPPER('café èñ ß') FROM RDB$DATABASE"))
        print()
        print('ORDER BY name_ci_ai:',
              '  '.join(r[0] for r in query(db, 'SELECT name_ci_ai FROM t ORDER BY name_ci_ai')))
        print('ORDER BY name_bin  :',
              '  '.join(r[0] for r in query(db, 'SELECT name_bin FROM t ORDER BY name_bin')),
              '   (binary: uppercase codepoints first)\n')

        # -- 2./3. Same stored WIN1252 'Café', two connection charsets.
        print(f'{SELECT_WIN} - same row, two connections:')
        raw, shown = fetch_bytes(db)
        hexdump('charset=UTF8:', raw, shown)
        db.commit()
        with attach(path, charset='NONE') as none:
            raw, shown = fetch_bytes(none)
            hexdump('charset=NONE:', raw, shown)
            print('  (charset NONE decodes with the locale codec:', locale.getpreferredencoding() + ')')
        print('  -> the column stores E9 (WIN1252); the UTF8 connection receives the\n'
              '     transliterated C3 A9, the NONE connection the raw stored byte.')
    print('\ndone.')


if __name__ == '__main__':
    run(main)
