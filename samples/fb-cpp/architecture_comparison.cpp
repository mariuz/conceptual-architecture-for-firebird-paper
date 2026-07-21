/*
 *  architecture_comparison.cpp (fb-cpp) — one client library, two ways in.
 *
 *  The fb-cpp twin of ../cpp/architecture_comparison.cpp: the same two
 *  attachments — inet://localhost/employee through the Remote provider,
 *  a plain local path through the Engine provider loaded into this
 *  process — asked the same three questions (ENGINE_VERSION,
 *  NETWORK_PROTOCOL, MON$SERVER_PID vs getpid()).  fb-cpp adds a third
 *  loading model on top: Client{"fbclient"} resolves and loads the
 *  client library AT RUNTIME via Boost.DLL, so even libfbclient itself
 *  is a plugin here.  NETWORK_PROTOCOL being SQL NULL when embedded
 *  arrives as an empty std::optional — no null-indicator bookkeeping.
 *  See ../../architecture-comparison.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_architecture_comparison [remote-db] [embedded-path]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <string>
#include <unistd.h>

using namespace fbcpp;
using namespace fbcpp_sample;

static void inspect(Client& client, const char* label, const std::string& database, bool create)
{
	Attachment att = create
		? attachOrCreate(client, database)
		: Attachment{client, database, defaultOptions()};

	Transaction tra{att};
	Statement stmt{att, tra,
		"select rdb$get_context('SYSTEM', 'ENGINE_VERSION'), "
		"       rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), "
		"       a.mon$server_pid "
		"from mon$attachments a "
		"where a.mon$attachment_id = current_connection"};
	stmt.execute(tra);

	const std::string version = stmt.getString(0).value_or("<null>");
	const std::string protocol = stmt.getString(1).value_or("<null>");	// NULL <=> in-process
	const int serverPid = stmt.getInt32(2).value_or(-1);

	printf("%s\n", label);
	printf("    connection string : %s\n", database.c_str());
	printf("    ENGINE_VERSION    : %s\n", version.c_str());
	printf("    NETWORK_PROTOCOL  : %s\n", protocol.c_str());
	printf("    MON$SERVER_PID    : %d   (this process is pid %d%s)\n",
		serverPid, getpid(),
		serverPid == getpid() ? " -- the engine runs IN this process" : "");

	tra.commit();
}

int main(int argc, char** argv)
{
	const char* remoteDb = argOrDefault(argc, argv, 1,
		"inet://localhost/employee");
	const char* embeddedDb = argOrDefault(argc, argv, 2,
		"/tmp/fbhandson/arch_embedded_fbcpp.fdb");

	try
	{
		// Loading model 3: Boost.DLL dlopens libfbclient right here.
		Client client{"fbclient"};

		printf("One libfbclient, two providers behind the Y-valve.\n\n");
		inspect(client, "[1] Remote provider (client-server):", remoteDb, false);
		printf("\n");
		inspect(client, "[2] Engine provider (embedded, no server):", embeddedDb, true);
		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
