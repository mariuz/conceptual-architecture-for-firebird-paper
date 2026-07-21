/*
 *  fbcpp_sample.h — shared defaults for the fb-cpp hands-on twins.
 *
 *  These samples mirror the OO-API programs in ../cpp using fb-cpp
 *  (https://github.com/asfernandes/fb-cpp, vendored at ../../extern/fb-cpp),
 *  a modern C++20 wrapper over the same client API: RAII everywhere,
 *  std::optional for nullable values, builder-style option objects.
 *  fb-cpp needs almost no boilerplate, so unlike ../cpp/fb_sample.h this
 *  header only centralizes credentials and error reporting.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>

#include "fb-cpp/fb-cpp.h"

namespace fbcpp_sample
{

inline const char* envOr(const char* name, const char* dflt)
{
	const char* v = getenv(name);
	return v && *v ? v : dflt;
}

// Standard credentials, overridable via ISC_USER / ISC_PASSWORD.
inline fbcpp::AttachmentOptions defaultOptions()
{
	return fbcpp::AttachmentOptions()
		.setUserName(envOr("ISC_USER", "SYSDBA"))
		.setPassword(envOr("ISC_PASSWORD", "masterkey"));
}

// Attach to an existing database, or create it (8K pages come from the
// engine default) when the first attach fails.
inline fbcpp::Attachment attachOrCreate(fbcpp::Client& client, const std::string& database)
{
	try
	{
		return fbcpp::Attachment{client, database, defaultOptions()};
	}
	catch (const fbcpp::FbCppException&)
	{
		return fbcpp::Attachment{client, database,
			defaultOptions().setCreateDatabase(true)};
	}
}

inline const char* argOrDefault(int argc, char** argv, int n, const char* dflt)
{
	return argc > n ? argv[n] : dflt;
}

// fb-cpp surfaces the whole status vector through what(); every sample's
// main() ends with  } catch (const std::exception& e) { return report(e); }
inline int report(const std::exception& e)
{
	fprintf(stderr, "fb-cpp error:\n%s\n", e.what());
	return 1;
}

} // namespace fbcpp_sample
