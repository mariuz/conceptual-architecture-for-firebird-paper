/*
 *  api_styles.cpp (fb-cpp) — the THIRD API style over the same client library.
 *
 *  ../cpp/api_styles.cpp runs one identical SELECT through the legacy ISC API
 *  (hand-built DPB, status vectors, XSQLDA) and through the OO API
 *  (IXpbBuilder, ThrowStatusWrapper, IMessageMetadata).  This twin adds the
 *  third rung of the ladder: fb-cpp, a C++20 wrapper OVER the OO API —
 *  options objects instead of parameter blocks, RAII instead of release()
 *  bookkeeping, std::optional instead of null indicators, one typed
 *  exception hierarchy instead of two error models.  All three styles load
 *  the same fbclient and meet at the same Y-valve.
 *  See ../../client-apis-and-drivers.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_api_styles [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, "inet://localhost/employee");

	try
	{
		// The whole program: load fbclient, attach, one scalar query.
		// Compare with the ~70 lines of iscStyle() in ../cpp/api_styles.cpp.
		Client client{"fbclient"};
		Attachment att{client, database,
			defaultOptions().setConnectionCharSet("NONE")};  // stock employee.fdb is charset NONE
		Transaction tra{att};

		printf("[fb-cpp ] engine version = %s\n",
			att.queryScalar<std::string>(tra,
				"select rdb$get_context('SYSTEM', 'ENGINE_VERSION') from rdb$database")
				.value_or("?").c_str());

		tra.commit();
		printf("same engine, same Y-valve, a third API style. done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
