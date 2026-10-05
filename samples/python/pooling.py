#
# pooling.py - the external-connections (EDS) pool, watched live (Python twin
# of ../cpp/pooling.cpp; see ../../connection-pooling.md).
#
# Outbound: the server acts as a CLIENT of another data source through
# EXECUTE STATEMENT ... ON EXTERNAL and pools those connections.  The sample
# tunes the pool (ALTER EXTERNAL CONNECTIONS POOL, needs MODIFY_EXT_CONN_POOL),
# makes three external calls to the same DSN/user and reads the EXT_CONN_POOL_*
# context variables at each stage.  firebird-driver spells the two commit
# forms as commit(retaining=True) and commit(), so the sample also shows the
# subtlety the C++ comment describes: after COMMIT RETAINING the pooled
# connection is still ACTIVE; only a full commit parks it on the idle list.
#
# Inbound: firebird-driver ships no client-side pool (unlike node-firebird or
# fb-cpp), so - like the Rust twin - the sample shows what such a pool would
# cache: each connect() is a MON$ATTACHMENTS row, and close() really detaches.
#
# Run:  python3 pooling.py [database] [external-dsn]
#
import os
import sys

from fbsample import PASSWORD, USER, attach, execute, query, run

POOL_STATE = """
select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_SIZE'),
       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_LIFETIME'),
       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT'),
       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT')
from rdb$database"""


def pool_state(con, moment):
    size, lifetime, idle, active = query(con, POOL_STATE)[0]
    print(f'{moment:<23} size={size} lifetime={lifetime}s idle={idle} active={active}')


def attachments(con):
    """This process's attachments, seen from a fresh snapshot of MON$."""
    n = query(con, "select count(*) from mon$attachments"
                   " where mon$remote_pid = ?", (os.getpid(),))[0][0]
    con.commit()          # the next MON$ read starts a new snapshot
    return n


def main():
    database = sys.argv[1] if len(sys.argv) > 1 else 'employee'
    external = sys.argv[2] if len(sys.argv) > 2 else 'inet://localhost/employee'
    block = f"""
execute block returns (idle varchar(10), active varchar(10)) as
  declare i int = 0;
  declare v int;
begin
  while (i < 3) do
  begin
    execute statement 'select 1 from rdb$database'
      on external '{external}'
      as user '{USER}' password '{PASSWORD}'
      into :v;
    i = i + 1;
  end
  idle   = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');
  active = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');
  suspend;
end"""

    print('-- outbound: the server-side EDS pool --')
    with attach(database) as con:
        # 1. Tune the pool at runtime (per server process, not persistent).
        execute(con, 'alter external connections pool set size 5', commit=False)
        execute(con, 'alter external connections pool set lifetime 30 second',
                commit=False)
        con.commit(retaining=True)
        pool_state(con, 'before:')

        # 2. Three calls with the same (DSN, user, password, role) key.
        idle, active = query(con, block)[0]
        print(f'{"inside the block:":<23} idle={idle} active={active}'
              '   (3 calls, 1 outbound connection)')

        # 3. COMMIT RETAINING keeps the transaction context - and the
        #    pooled connection stays ACTIVE.
        con.commit(retaining=True)
        pool_state(con, 'after commit retaining:')

        # 4. A full commit: reset with ALTER SESSION RESET, parked as idle.
        con.commit()
        pool_state(con, 'after commit:')

        # 5. Evict every idle connection now.
        execute(con, 'alter external connections pool clear all', commit=False)
        pool_state(con, 'after CLEAR ALL:')
        con.commit()

    print('\n-- inbound: what a client-side pool would cache --')
    with attach(database) as watcher:
        with attach(database) as a, attach(database) as b:
            ids = [query(c, 'select current_connection from rdb$database')[0][0]
                   for c in (a, b)]
            print(f'opened attachments {ids[0]} and {ids[1]}:'
                  f' {attachments(watcher) - 1} extra rows in MON$ATTACHMENTS')
            again = [query(a, 'select current_connection from rdb$database')[0][0]
                     for _ in range(3)]
            print(f'three queries on one attachment: CURRENT_CONNECTION = {again}')
            a.commit()
            a.close()
            print(f'a.close(): {attachments(watcher) - 1} extra row left'
                  ' -- close() really detached (a pool would have kept it)')
            b.commit()
    print('done.')


if __name__ == '__main__':
    run(main)
