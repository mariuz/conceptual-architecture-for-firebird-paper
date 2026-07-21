/*
 *  threading.cpp (fb-cpp) — SuperServer's thread topology from the outside.
 *
 *  The fb-cpp twin of ../cpp/threading.cpp: MON$SERVER_PID names the engine
 *  process, /proc/<pid>/task counts its threads, twelve client threads each
 *  hold an attachment open for two seconds, and MON$ATTACHMENTS lists the
 *  background workers as system attachments.  Each client thread builds its
 *  own Client + Attachment — four RAII objects per thread replace the
 *  OO-API version's manual dispatcher/DPB/status lifecycle.
 *  See ../../threading-and-synchronization.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_threading [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/threading_fbcpp.fdb";

static int countThreads(const std::string& pid)
{
	int n = 0;
	if (DIR* d = opendir(("/proc/" + pid + "/task").c_str()))
	{
		while (dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				++n;
		closedir(d);
	}
	return n;
}

// Print a result set the way isql would, taking column names from the
// statement's output descriptors and every value as an optional string.
static void printTable(Attachment& att, Transaction& tra, const char* sql)
{
	Statement stmt{att, tra, sql};
	const auto& cols = stmt.getOutputDescriptors();
	std::vector<std::string> names;
	for (const auto& d : cols)
		names.push_back(d.alias.empty() ? d.name : d.alias);

	std::vector<std::vector<std::string>> rows;
	for (bool ok = stmt.execute(tra); ok; ok = stmt.fetchNext())
	{
		std::vector<std::string> row;
		for (unsigned i = 0; i < cols.size(); ++i)
			row.push_back(stmt.getString(i).value_or("<null>"));
		rows.push_back(std::move(row));
	}

	std::vector<size_t> w;
	for (size_t c = 0; c < names.size(); ++c)
	{
		size_t m = names[c].size();
		for (auto& r : rows)
			m = std::max(m, r[c].size());
		w.push_back(m);
	}
	for (size_t c = 0; c < names.size(); ++c)
		printf("%-*s ", (int) w[c], names[c].c_str());
	printf("\n");
	for (size_t c = 0; c < names.size(); ++c)
		printf("%s ", std::string(w[c], '-').c_str());
	printf("\n");
	for (auto& r : rows)
	{
		for (size_t c = 0; c < r.size(); ++c)
			printf("%-*s ", (int) w[c], r[c].c_str());
		printf("\n");
	}
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);
		{
			Transaction t{att};
			att.execute(t, "recreate table t (id int primary key, v int)");
			t.commit();
			Transaction t2{att};
			att.execute(t2, "update or insert into t values (1, 0) matching (id)");
			t2.commit();
		}

		std::string pid;
		{
			Transaction t{att};
			pid = att.queryScalar<std::string>(t,
				"select MON$SERVER_PID from MON$ATTACHMENTS "
				"where MON$ATTACHMENT_ID = current_connection").value();
			t.commit();
		}
		printf("engine process: pid %s, %d threads (1 attachment open)\n",
			pid.c_str(), countThreads(pid));

		// Twelve attachments from twelve threads of THIS process; each holds
		// its attachment open for 2 s.  Server side: one thread each (drawn
		// from the pool when idle threads exist, created otherwise).
		std::vector<std::thread> workers;
		for (int i = 0; i < 12; ++i)
			workers.emplace_back([database]() {
				Client c{"fbclient"};
				Attachment w{c, database, defaultOptions()};
				Transaction wt{w};
				w.queryScalar<std::int64_t>(wt, "select count(*) from t");
				wt.commit();
				std::this_thread::sleep_for(std::chrono::seconds(2));
			});

		std::this_thread::sleep_for(std::chrono::seconds(1));
		{
			Transaction t{att};
			const auto users = att.queryScalar<std::int64_t>(t,
				"select count(*) from MON$ATTACHMENTS where MON$SYSTEM_FLAG = 0").value();
			const auto pids = att.queryScalar<std::int64_t>(t,
				"select count(distinct MON$SERVER_PID) from MON$ATTACHMENTS").value();
			t.commit();
			printf("with 12 extra attachments: %d threads | %lld user attachments, "
				"%lld distinct server pid\n", countThreads(pid),
				(long long) users, (long long) pids);
		}

		for (auto& w : workers)
			w.join();
		std::this_thread::sleep_for(std::chrono::seconds(1));
		printf("after they detach:        %d threads (pooled, not destroyed)\n",
			countThreads(pid));

		// The engine's own workers hold real attachments, visible from SQL.
		Transaction t{att};
		printTable(att, t,
			"select MON$ATTACHMENT_ID, MON$SYSTEM_FLAG, trim(MON$USER), "
			"       coalesce(MON$REMOTE_PROCESS, '<internal>') "
			"from MON$ATTACHMENTS order by MON$ATTACHMENT_ID");
		t.commit();
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
