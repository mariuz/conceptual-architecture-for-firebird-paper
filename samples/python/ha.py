#
# ha.py - the one HA primitive that is pure client-side SQL: a database
# SHADOW (Python twin of ../cpp/ha.cpp; see ../../high-availability.md).
#
# On a fresh scratch database: CREATE SHADOW, see it registered in RDB$FILES
# (flag 1 = shadow), stat() both files - server and sample share a host
# here - as 5000 rows are inserted, then DROP SHADOW 1 DELETE FILE.
# CREATE/DROP SHADOW are ordinary DSQL, so firebird-driver needs nothing
# special: recreate() starts from a fresh database (dropping a database
# drops its shadow too), and os.stat plays stat().  Replica promotion and
# sync_replica need server-side configuration and stay as text in the
# document.
#
# Run:  python3 ha.py [database]
#
import os

from fbsample import db_path, execute, query, recreate, run, scalar

MAIN = db_path('ha')                         # server-side paths
SHADOW = os.path.splitext(MAIN)[0] + '.shd'


def file_size(path):
    try:
        return os.stat(path).st_size
    except OSError:
        return -1


def show_files(when):
    print(f'{when:<28} main = {file_size(MAIN):8} bytes, '
          f'shadow = {file_size(SHADOW):8} bytes')


def main():
    with recreate(MAIN) as con:
        execute(con, 'CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, '
                     'PAYLOAD VARCHAR(200))')

        # 1. Create the synchronous page-level mirror.
        execute(con, f"CREATE SHADOW 1 '{SHADOW}'")
        print('CREATE SHADOW 1 done - the engine dumped every page to the mirror\n')

        # The shadow is registered in the metadata like any other file.
        for name, number, flags in query(con,
                'SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS '
                'FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER'):
            print(f'RDB$FILES: {name.strip()}  shadow_number={number}  flags={flags}')
        print()
        show_files('after CREATE SHADOW:')

        # 2. Write load: every page write now goes to both files.
        execute(con, "EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
                     "  WHILE (I < 5000) DO BEGIN "
                     "    INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1; "
                     "  END "
                     "END")
        show_files('after 5000 inserts:')

        # 3. Retire the mirror.
        execute(con, 'DROP SHADOW 1 DELETE FILE')
        print('\nDROP SHADOW 1 DELETE FILE done')
        show_files('after DROP SHADOW:')

        print('\nRDB$FILES rows left:', scalar(con, 'SELECT COUNT(*) FROM RDB$FILES'))
        con.commit()
    print('done.')


if __name__ == '__main__':
    run(main)
