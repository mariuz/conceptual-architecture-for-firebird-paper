/*
 *  architecture_comparison.cpp — one client library, two ways into the engine.
 *
 *  Companion sample for ../../architecture-comparison.md.  The comparison
 *  table's most unusual row is Firebird's: "client-server AND embedded,
 *  same engine".  This program proves it with a single binary linked only
 *  against libfbclient:
 *
 *    1. attach inet://localhost/employee      — the Y-valve picks the Remote
 *       provider; SQL travels over the wire to the server process.
 *    2. attach /tmp/fbhandson/arch_embedded.fdb (a plain local path; created
 *       on first run) — the Y-valve loads the Engine provider INTO THIS
 *       PROCESS; the same libfbclient is now a complete database engine and
 *       no server is involved at all.
 *
 *  For each attachment we ask the engine itself where we are:
 *    - ENGINE_VERSION            — same engine code both ways
 *    - NETWORK_PROTOCOL          — TCPv4/TCPv6 remotely, NULL embedded
 *    - MON$SERVER_PID vs getpid()— the server's process remotely,
 *                                  OUR OWN pid when embedded
 */

#include "fb_sample.h"
#include <unistd.h>

using namespace fbsample;

static void inspect(const char* label, const char* database, bool create)
{
	Db db;
	if (create)
		db.attachOrCreate(database);
	else
		db.attach(database);

	ITransaction* tra = db.start();

	Db::Table t = db.query(tra,
		"select rdb$get_context('SYSTEM', 'ENGINE_VERSION'), "
		"       rdb$get_context('SYSTEM', 'NETWORK_PROTOCOL'), "
		"       a.mon$server_pid "
		"from mon$attachments a "
		"where a.mon$attachment_id = current_connection");

	const std::string& version = t.rows[0][0];
	const std::string& protocol = t.rows[0][1];
	const std::string& serverPid = t.rows[0][2];

	printf("%s\n", label);
	printf("    connection string : %s\n", database);
	printf("    ENGINE_VERSION    : %s\n", version.c_str());
	printf("    NETWORK_PROTOCOL  : %s\n", protocol.c_str());
	printf("    MON$SERVER_PID    : %s   (this process is pid %d%s)\n",
		serverPid.c_str(), getpid(),
		serverPid == std::to_string(getpid())
			? " -- the engine runs IN this process" : "");

	tra->commit(&db.status);
}

int main(int argc, char** argv)
{
	const char* remoteDb = argOrDefault(argc, argv, 1,
		"inet://localhost/employee");
	const char* embeddedDb = argOrDefault(argc, argv, 2,
		"/tmp/fbhandson/arch_embedded.fdb");

	try
	{
		printf("One libfbclient, two providers behind the Y-valve.\n\n");
		inspect("[1] Remote provider (client-server):", remoteDb, false);
		printf("\n");
		inspect("[2] Engine provider (embedded, no server):", embeddedDb, true);
		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return report(e);
	}
}
