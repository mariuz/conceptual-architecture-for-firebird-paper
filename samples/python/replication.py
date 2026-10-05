#
# replication.py - the client-visible half of Firebird replication: the
# publication (Python twin of ../cpp/replication.cpp; see
# ../../replication-architecture.md).
#
# All of it is plain DDL plus system tables - no replication.conf, no
# restart:
#
#   ALTER DATABASE ENABLE PUBLICATION              -> RDB$PUBLICATIONS
#   ALTER DATABASE INCLUDE TABLE ... / INCLUDE ALL -> RDB$PUBLICATION_TABLES
#
# The replica side is asked twice: through SQL (MON$DATABASE.MON$REPLICA_MODE)
# and through the attachment's info call - firebird-driver's
# Connection.info.get_info(DbInfoCode.REPLICA_MODE) decodes
# isc_info_replica_mode into the typed ReplicaMode enum.  The journal /
# segment transport behind it needs server-side replication.conf and stays
# as text in the document's walk-through.
#
# Run:  python3 replication.py [database]
#
from firebird.driver import DatabaseError, DbInfoCode

from fbsample import attach_or_create, db_path, execute, query, run, scalar


def pub_state(db, when):
    print('--', when)
    for name, active, auto in query(db, 'SELECT TRIM(RDB$PUBLICATION_NAME), RDB$ACTIVE_FLAG, '
                                        'RDB$AUTO_ENABLE FROM RDB$PUBLICATIONS'):
        tables = [f'{s}.{t}' for s, t in query(
            db, 'SELECT TRIM(RDB$TABLE_SCHEMA_NAME), TRIM(RDB$TABLE_NAME) '
                'FROM RDB$PUBLICATION_TABLES ORDER BY RDB$TABLE_NAME')]
        print(f'{name:<13} ACTIVE_FLAG {active}   AUTO_ENABLE {auto}    '
              f'published: {", ".join(tables) or "(none)"}')


def main():
    with attach_or_create(db_path('replication')) as db:
        # Idempotent reset: back to a clean, unpublished state.
        for sql in ('ALTER DATABASE EXCLUDE ALL FROM PUBLICATION',
                    'ALTER DATABASE DISABLE PUBLICATION',
                    'DROP TABLE REPL_ORDERS', 'DROP TABLE REPL_SCRATCH'):
            try:
                execute(db, sql)
            except DatabaseError:
                db.rollback()
        execute(db, 'CREATE TABLE REPL_ORDERS (ID INT NOT NULL PRIMARY KEY, ITEM VARCHAR(30))')
        execute(db, 'CREATE TABLE REPL_SCRATCH (N INT)')        # note: no key

        pub_state(db, 'initial state (publication exists but is inactive)')
        execute(db, 'ALTER DATABASE ENABLE PUBLICATION')
        pub_state(db, 'after ENABLE PUBLICATION')
        execute(db, 'ALTER DATABASE INCLUDE TABLE REPL_ORDERS TO PUBLICATION')
        pub_state(db, 'after INCLUDE TABLE REPL_ORDERS')
        execute(db, 'ALTER DATABASE INCLUDE ALL TO PUBLICATION')
        pub_state(db, 'after INCLUDE ALL (auto-enable: future tables join automatically)')

        # The other end of the relationship: is this database a replica?
        print('\nMON$DATABASE.MON$REPLICA_MODE         =',
              scalar(db, 'SELECT MON$REPLICA_MODE FROM MON$DATABASE'))
        print('info.get_info(DbInfoCode.REPLICA_MODE) =',
              repr(db.info.get_info(DbInfoCode.REPLICA_MODE)))
        db.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
