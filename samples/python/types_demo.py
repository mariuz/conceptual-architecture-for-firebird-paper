#
# types_demo.py - Firebird's headline types round-tripped from Python
# (Python twin of ../cpp/types.cpp; see ../../sql-dialect-and-types.md).
# Not named types.py: that would shadow the standard library module.
#
# One table: BOOLEAN, INT128 at its maximum, DECFLOAT(34) holding an exact
# 0.1, TIMESTAMP WITH TIME ZONE with a named zone, and a CHECK-constrained
# domain (an invalid insert shows the CHECK travelling with the type).
# Shown: the wire type codes from the output metadata, then every value
# fetched *typed*.
#
# firebird-driver decodes every one of them natively: BOOLEAN -> bool,
# INT128 -> int (Python ints are unbounded, so 2**127 - 1 compares exactly),
# DECFLOAT(34) -> decimal.Decimal (exact 0.1), TIMESTAMP WITH TIME ZONE ->
# an aware datetime keeping the zone name.  The wire codes come from the
# driver's per-column ItemMetadata (SQLDataType enum); the driver marks that
# dataclass internal and exposes it only as Statement._out_desc - the
# public cursor.description carries the *Python* type instead.
#
# Run:  python3 types_demo.py [database]
#
from decimal import Decimal

from firebird.driver import DatabaseError

from fbsample import attach_or_create, db_path, error_text, execute, run


def main():
    with attach_or_create(db_path('types')) as db:
        # Idempotent cleanup, each drop committed on its own (DFW).
        for sql in ('DROP TABLE showcase', 'DROP DOMAIN d_email'):
            try:
                execute(db, sql)
            except DatabaseError:
                db.rollback()

        execute(db, "CREATE DOMAIN d_email AS VARCHAR(60) CHECK (VALUE LIKE '%@%')")
        execute(db, 'CREATE TABLE showcase ('
                    '  flag  BOOLEAN,'
                    '  big   INT128,'
                    '  money DECFLOAT(34),'
                    '  born  TIMESTAMP WITH TIME ZONE,'
                    '  mail  d_email)')

        cur = db.cursor()
        cur.execute("INSERT INTO showcase VALUES ("
                    "  TRUE,"
                    "  170141183460469231731687303715884105727,"
                    "  0.1,"
                    "  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
                    "  'user@example.com')")
        try:
            cur.execute("INSERT INTO showcase (mail) VALUES ('not-an-address')")
            print('BUG: domain CHECK did not fire')
        except DatabaseError as e:
            print("domain CHECK rejected 'not-an-address':")
            print(f'    sqlcode {e.sqlcode}, gds {e.gds_codes[0]}:',
                  error_text(e).splitlines()[-1][:100])

        # What the wire carries, and what Python gets.
        stmt = cur.prepare('SELECT * FROM showcase')
        cur.execute(stmt)
        print('\ncolumn  wire type (ItemMetadata.datatype)   python type')
        print('------  ------------------------------   -----------')
        for meta, desc in zip(stmt._out_desc, cur.description):
            print(f'{meta.field:<7} {meta.datatype.value:5d} = {meta.datatype.name:<20}'
                  f'   {getattr(desc[1], "__name__", desc[1])}')
        flag, big, money, born, mail = cur.fetchone()
        stmt.free()

        print('\ntyped round-trip:')
        print(f'  FLAG  {flag!r}')
        print(f'  BIG   {big}  == 2**127 - 1 ? {big == 2**127 - 1}')
        print(f'  MONEY {money!r}  == Decimal("0.1") exactly ? {money == Decimal("0.1")}'
              f'  (and 0.1 float? {money == 0.1})')
        print(f'  BORN  {born.isoformat()}  zone={born.tzinfo._timezone_}')
        print(f'  MAIL  {mail!r}')
        cur.close()
        db.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
