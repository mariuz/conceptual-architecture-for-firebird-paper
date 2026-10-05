#
# schemas.py - schemas and the search path (Python twin of
# ../cpp/schemas.cpp; see ../../schemas-and-name-resolution.md).
#
# The five demonstrations: RDB$SCHEMAS and the default path; one unqualified
# SELECT resolving to PUBLIC.CUSTOMERS or APP.CUSTOMERS as SET SEARCH_PATH
# changes; SYSTEM auto-appended; a procedure created while APP leads the path
# keeping APP.CUSTOMERS after the session flips to PUBLIC (RDB$DEPENDENCIES
# records it); and the schema-qualified plan.
#
# Resolution is server-side, so nothing here needs driver support - except
# the plan, which firebird-driver exposes on a prepared Statement as both
# .plan (the legacy one-line PLAN) and .detailed_plan (the explained tree).
# The helper commits after each DDL statement, so the path surviving those
# commits shows it is attachment state, not transaction state.
#
# Run:  python3 schemas.py [database]
#
from firebird.driver import DatabaseError

from fbsample import attach_or_create, db_path, execute, query, run, scalar

PATH = "select rdb$get_context('SYSTEM', 'SEARCH_PATH') from rdb$database"
WHICH = 'select origin from customers'


def main():
    with attach_or_create(db_path('schemas')) as db:
        # -- Idempotent cleanup + setup.
        for sql in ('drop procedure app.which_one', 'drop table public.customers',
                    'drop table app.customers', 'drop schema app'):
            try:
                execute(db, sql)
            except DatabaseError:
                db.rollback()
        execute(db, 'create schema app')
        execute(db, 'create table public.customers (id int, origin varchar(20))')
        execute(db, 'create table app.customers    (id int, origin varchar(20))')
        execute(db, "insert into public.customers values (1, 'from PUBLIC')")
        execute(db, "insert into app.customers    values (2, 'from APP')")

        # -- 1. The catalog and the default path.
        schemas = query(db, 'select trim(rdb$schema_name) from rdb$schemas order by 1')
        print('schemas in RDB$SCHEMAS      :', '  '.join(r[0] for r in schemas))
        print('default search path         :', scalar(db, PATH))

        # -- 2. Same statement, two resolutions.
        print('\nSELECT ORIGIN FROM CUSTOMERS, as the path changes:')
        print('  path PUBLIC,SYSTEM        ->', scalar(db, WHICH))
        execute(db, 'set search_path to app, public')
        print('  path APP,PUBLIC           ->', scalar(db, WHICH))

        # -- 3. SYSTEM can be moved but not removed.
        execute(db, 'set search_path to app')
        print('\nSET SEARCH_PATH TO APP      ->', scalar(db, PATH),
              '  (SYSTEM auto-appended)')

        # -- 4. Stored code binds its own schema, not the caller's path.
        execute(db, 'set search_path to app, public')
        execute(db, 'create procedure which_one returns (src varchar(20)) as '
                    'begin select origin from customers into :src; suspend; end')
        print('\nprocedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)')
        execute(db, 'set search_path to public')
        print('  after SET SEARCH_PATH TO PUBLIC:')
        print('    direct SELECT ... FROM CUSTOMERS ->', scalar(db, WHICH))
        print('    SELECT SRC FROM APP.WHICH_ONE    ->',
              scalar(db, 'select src from app.which_one'), '  <- unmoved')
        print('    RDB$DEPENDENCIES records         ->', scalar(
            db, "select trim(rdb$depended_on_schema_name) || '.' || trim(rdb$depended_on_name)"
                "  from rdb$dependencies where rdb$dependent_name = 'WHICH_ONE'"))

        # -- 5. Plans are schema-qualified: legacy and explained forms.
        cur = db.cursor()
        stmt = cur.prepare('select count(*) from customers')
        print('\nplan for unqualified SELECT :', stmt.plan.strip())
        print('detailed_plan               :',
              stmt.detailed_plan.strip().replace('\n', '\n' + ' ' * 30))
        stmt.free()
        cur.close()
        db.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
