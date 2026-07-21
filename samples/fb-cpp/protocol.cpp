/*
 *  protocol.cpp (fb-cpp) — the negotiated wire session, reported by the engine.
 *
 *  The fb-cpp twin of ../protocol_client.cpp: attach over inet:// and let
 *  fbclient run the whole op_connect / Srp256 / op_crypt handshake, then ask
 *  the engine what was actually negotiated.  Where the OO-API version builds
 *  a DPB with IXpbBuilder and hand-decodes a VARCHAR from the fetch buffer
 *  (2-byte length prefix, trailing blanks), here the DPB is AttachmentOptions
 *  and each answer is queryScalar<std::string> returning std::optional.
 *  Same handshake on the wire — see ../../firebird-wire-protocol.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_protocol [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>

using namespace fbcpp;
using namespace fbcpp_sample;

// One RDB$GET_CONTEXT('SYSTEM', ...) answer; NULL means "not applicable"
// (e.g. WIRE_CRYPT_PLUGIN on an unencrypted or embedded connection).
static std::string context(Attachment& att, Transaction& tra, const char* var)
{
	return att.queryScalar<std::string>(tra,
		std::string("select rdb$get_context('SYSTEM', '") + var + "') from rdb$database")
		.value_or("(none)");
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost/employee");

	try
	{
		Client client{"fbclient"};

		// The single constructor that performs the whole handshake shown
		// byte-by-byte in ../nodejs/srp-handshake.js.
		Attachment att{client, database,
			defaultOptions().setConnectionCharSet("NONE")};  // stock employee.fdb is charset NONE
		printf("attached to %s\n", database);

		Transaction tra{att};
		printf("engine version : %s\n", context(att, tra, "ENGINE_VERSION").c_str());
		printf("protocol       : %s\n", context(att, tra, "NETWORK_PROTOCOL").c_str());
		printf("wire crypt     : %s\n", context(att, tra, "WIRE_CRYPT_PLUGIN").c_str());
		printf("authenticated  : %s\n", att.queryScalar<std::string>(tra,
			"select trim(current_user) from rdb$database").value_or("?").c_str());
		tra.commit();

		att.disconnect();
		printf("detached. bye\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
