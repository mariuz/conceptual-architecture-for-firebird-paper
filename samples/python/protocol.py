#
# protocol.py - the negotiated wire session, reported by the engine (Python
# twin of ../protocol_client.cpp and ../fpc/protocol.pas; see
# ../../firebird-wire-protocol.md).
#
# Attach over inet:// and let libfbclient's Remote provider run the whole
# op_connect / op_cond_accept / Srp256 / op_crypt handshake, then ask the
# engine what was actually negotiated - first through the SYSTEM context
# variables the C++ sample reads, then through the attachment's own
# MON$ATTACHMENTS row, as the Free Pascal twin does.
#
# firebird-driver has no wire implementation of its own: unlike node-firebird
# and rsfbclient-rust (which re-implement the protocol and negotiate older
# versions with Arc4), it is a ctypes binding of libfbclient, so its session
# is the C++ clients' session.  It can also read the client-side view without
# a query: con.info.firebird_version returns one line per layer, the Remote
# layers stamped with transport, host and protocol version (".../tcp (host)/P20").
#
# Run:  python3 protocol.py [database]
#
import sys

from firebird.driver import connect

from fbsample import PASSWORD, USER, dsn, query, run


def main():
    database = sys.argv[1] if len(sys.argv) > 1 else dsn('employee')
    # charset NONE, as in the C++ sample: the stock employee.fdb is NONE
    with connect(database, user=USER, password=PASSWORD, charset='NONE') as con:
        print('attached to', database)

        def ctx(name):
            return query(con, f"select rdb$get_context('SYSTEM', '{name}')"
                              " from rdb$database")[0][0]

        print('engine version :', ctx('ENGINE_VERSION'))
        print('protocol       :', ctx('NETWORK_PROTOCOL'))
        print('wire crypt     :', ctx('WIRE_CRYPT_PLUGIN') or '(none)')
        print('authenticated  :', query(con, 'select trim(current_user)'
                                             ' from rdb$database')[0][0])

        (auth, remote_version, crypt, client_version) = query(con,
            'SELECT MON$AUTH_METHOD, MON$REMOTE_VERSION, MON$WIRE_CRYPT_PLUGIN,'
            '       MON$CLIENT_VERSION FROM MON$ATTACHMENTS'
            ' WHERE MON$ATTACHMENT_ID = CURRENT_CONNECTION')[0]
        con.commit()
        print('\nMON$ATTACHMENTS, as the server recorded the handshake:')
        print(f'   auth method    : {auth}   <- SRP proof, password never sent')
        print(f'   wire protocol  : {remote_version}   <- highest version both sides speak')
        print(f'   wire crypt     : {crypt}   <- keyed from the SRP session key')
        print(f'   client version : {client_version}')

        print('\nthe client side, without a query (isc_info_firebird_version):')
        for layer in con.info.firebird_version.split('\n'):
            print('   ' + layer)
    print('detached. bye')


if __name__ == '__main__':
    run(main)
