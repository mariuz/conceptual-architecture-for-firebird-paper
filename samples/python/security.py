#
# security.py - the security layers observed from client code (Python twin
# of ../cpp/security.cpp; see ../../security-architecture.md).
#
# In one run: (1) WHO AM I - this attachment's own MON$ATTACHMENTS row (auth
# plugin and wire-crypt plugin, layers 1 and 2 as the server recorded them);
# (2) SEC$USERS, the virtual view over the security database; (3) least
# privilege - a temporary user plus a role carrying the MONITOR_ANY_ATTACHMENT
# system privilege, connecting without and with the role; (4) the error chain
# a wrong password produces.  The user and role are dropped at the end.
#
# firebird-driver puts the role in connect(role=...), which writes the same
# isc_dpb_sql_role_name the C++ sample inserts into its DPB by hand, and the
# failed login is a DatabaseError carrying sqlcode and the gds codes.  User
# management is deferred work executed at COMMIT, so every CREATE/DROP USER
# batch gets its own transaction and a full commit, as in the other twins.
# Own names (PY_USER, PY_MONITOR) so the twins can run side by side.
#
# Run:  python3 security.py [database]
#
from firebird.driver import DatabaseError, connect

from fbsample import (attach_or_create, db_path, dsn, error_text, execute,
                      query, run, scalar)

TMP_USER = 'PY_USER'
TMP_PASS = 'Hands0nPw'
ROLE = 'PY_MONITOR'

WHO = """select trim(mon$user), mon$auth_method, mon$wire_crypt_plugin,
                mon$remote_protocol, trim(coalesce(current_role, 'NONE'))
           from mon$attachments where mon$attachment_id = current_connection"""
VISIBLE = 'select count(*) from mon$attachments where mon$system_flag = 0'


def who_am_i(con, label):
    user, auth, crypt, proto, role = query(con, WHO)[0]
    print(f'{label:<22} user={user} auth={auth} wirecrypt={crypt} '
          f'protocol={proto} role={role}')
    con.commit()


def exec_ignore(con, sql):
    """Idempotent cleanup: DROP USER of a missing user fails only at COMMIT."""
    try:
        execute(con, sql)
    except DatabaseError:
        con.rollback()


def login(path, password=TMP_PASS, role=None):
    return connect(dsn(path), user=TMP_USER, password=password, role=role,
                   charset='UTF8')


def main():
    path = db_path('security')
    with attach_or_create(path) as admin:
        # 1. Layers 1+2, as recorded for THIS attachment.
        who_am_i(admin, 'admin attachment:')

        # 2+3. Temporary user (security database) and privileged role (this
        #      database), each batch with a full commit.
        exec_ignore(admin, f'drop user {TMP_USER} using plugin Srp')
        exec_ignore(admin, f'drop role {ROLE}')
        execute(admin, f"create user {TMP_USER} password '{TMP_PASS}' using plugin Srp")
        execute(admin, f'create role {ROLE} set system privileges to MONITOR_ANY_ATTACHMENT',
                commit=False)
        execute(admin, f'grant {ROLE} to user {TMP_USER}')

        print('\nSEC$USERS (the security database, through the virtual view):')
        print(f'    {"USER":<16} {"PLUGIN":<8} ADMIN')
        for name, plugin, is_admin in query(
                admin, 'select trim(sec$user_name), trim(sec$plugin), sec$admin'
                       '  from sec$users order by 1'):
            print(f'    {name:<16} {plugin:<8} {is_admin}')

        print(f'\nadmin sees {scalar(admin, VISIBLE)} user attachments in MON$ATTACHMENTS')
        admin.commit()

        with login(path) as plain:
            who_am_i(plain, 'user, no role:')
            print(f'  -> sees {scalar(plain, VISIBLE)} attachment(s): only its own')
            plain.commit()
        with login(path, role=ROLE) as monitor:
            who_am_i(monitor, 'user + role:')
            print(f'  -> sees {scalar(monitor, VISIBLE)} attachments: '
                  'MONITOR_ANY_ATTACHMENT at work')
            monitor.commit()

        # 4. The failed login, and its error chain.
        print('\nfailed login (wrong password) produces:')
        try:
            login(path, password='wrong-password').close()
        except DatabaseError as e:
            print('    sqlcode', e.sqlcode, '/ gds', e.gds_codes)
            print('    ' + error_text(e).replace('\n', '\n    '))

        execute(admin, f'drop user {TMP_USER} using plugin Srp')
        execute(admin, f'drop role {ROLE}')
    print('\ntemporary user and role dropped. done.')


if __name__ == '__main__':
    run(main)
