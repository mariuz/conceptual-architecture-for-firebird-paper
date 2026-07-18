/*
 *  protocol_client.cpp  --  the C++ counterpart to nodejs/query.js.
 *
 *  Where nodejs/srp-handshake.js re-implements the Firebird wire protocol
 *  by hand, this sample uses the official OO API and lets the fbclient
 *  library's REMOTE provider do the op_connect / Srp256 / op_crypt dance for
 *  us.  It then asks the *engine* (through CURRENT_USER and the SYSTEM
 *  RDB$GET_CONTEXT namespace) to report which protocol, authentication and
 *  wire-encryption were actually negotiated, so you can compare the two
 *  routes to the same server.
 *
 *  Because the connection string is a network URL (inet://host/db), the call
 *  path is: fbclient -> Y-valve -> Remote provider -> TCP -> server -> Y-valve
 *  -> Engine provider (see Figure 1 of the paper, and Figure 6 for the
 *  provider split).  Contrast client_test.cpp, which attaches to a local file
 *  and is served by the embedded Engine provider in-process.
 *
 *  Build (see samples/README.md and CMakeLists.txt):
 *      c++ -I<firebird>/include protocol_client.cpp -lfbclient -o protocol_client
 *
 *  Usage:
 *      ./protocol_client [inet://localhost/employee] [user] [password]
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include <firebird/Interface.h>

using namespace Firebird;

static IMaster* master = fb_get_master_interface();

// Fetch a single VARCHAR/text column produced by a one-row SELECT and return
// it trimmed.  Kept tiny on purpose; the OO-API fetch mechanics are shown in
// full in client_test.cpp.
static std::string queryScalar(ThrowStatusWrapper& st, IAttachment* att,
                               ITransaction* tra, const char* sql)
{
    IUtil* utl = master->getUtilInterface();
    IResultSet* rs = att->openCursor(&st, tra, 0, sql, SQL_DIALECT_CURRENT,
                                     nullptr, nullptr, nullptr, nullptr, 0);
    IMessageMetadata* meta = rs->getMetadata(&st);
    const unsigned len = meta->getMessageLength(&st);
    const unsigned off = meta->getOffset(&st, 0);
    const unsigned nullOff = meta->getNullOffset(&st, 0);

    std::string out;
    char* buf = new char[len];
    if (rs->fetchNext(&st, buf) == IStatus::RESULT_OK)
    {
        if (*reinterpret_cast<short*>(buf + nullOff) == 0)  // not NULL
        {
            // VARCHAR on the wire is a 2-byte length followed by the bytes.
            const char* v = buf + off;
            unsigned n = *reinterpret_cast<const unsigned short*>(v);
            out.assign(v + 2, n);
            while (!out.empty() && out.back() == ' ') out.pop_back();
        }
    }
    delete[] buf;
    rs->close(&st);
    meta->release();
    return out;
}

int main(int argc, char** argv)
{
    const char* database = (argc > 1) ? argv[1] : "inet://localhost/employee";
    const char* user = (argc > 2) ? argv[2] : "SYSDBA";
    const char* password = (argc > 3) ? argv[3] : "masterkey";

    ThrowStatusWrapper status(master->getStatus());
    IProvider* prov = master->getDispatcher();
    IUtil* utl = master->getUtilInterface();

    IAttachment* att = nullptr;
    ITransaction* tra = nullptr;
    IXpbBuilder* dpb = nullptr;
    int rc = 0;

    try
    {
        dpb = utl->getXpbBuilder(&status, IXpbBuilder::DPB, nullptr, 0);
        dpb->insertString(&status, isc_dpb_user_name, user);
        dpb->insertString(&status, isc_dpb_password, password);
        dpb->insertString(&status, isc_dpb_lc_ctype, "NONE"); // stock employee.fdb is charset NONE

        // The single call that performs the whole handshake demonstrated
        // byte-by-byte in nodejs/srp-handshake.js.
        att = prov->attachDatabase(&status, database, dpb->getBufferLength(&status),
                                   dpb->getBuffer(&status));
        std::printf("attached to %s\n", database);

        tra = att->startTransaction(&status, 0, nullptr);

        std::printf("engine version : %s\n", queryScalar(status, att, tra,
            "select rdb$get_context('SYSTEM','ENGINE_VERSION') from rdb$database").c_str());
        std::printf("protocol       : %s\n", queryScalar(status, att, tra,
            "select rdb$get_context('SYSTEM','NETWORK_PROTOCOL') from rdb$database").c_str());
        std::string wc = queryScalar(status, att, tra,
            "select rdb$get_context('SYSTEM','WIRE_CRYPT_PLUGIN') from rdb$database");
        std::printf("wire crypt     : %s\n", wc.empty() ? "(none)" : wc.c_str());
        std::printf("authenticated  : %s\n", queryScalar(status, att, tra,
            "select trim(current_user) from rdb$database").c_str());

        tra->commit(&status);
        tra = nullptr;
        att->detach(&status);
        att = nullptr;
        std::printf("detached. bye\n");
    }
    catch (const FbException& e)
    {
        char msg[512];
        utl->formatStatus(msg, sizeof(msg), e.getStatus());
        std::fprintf(stderr, "ERROR: %s\n", msg);
        rc = 1;
    }

    if (tra) tra->release();
    if (att) att->release();
    if (dpb) dpb->dispose();
    prov->release();
    status.dispose();
    return rc;
}
