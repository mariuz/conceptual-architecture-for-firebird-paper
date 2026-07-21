/*
 *  threading.cpp — companion sample for threading-and-synchronization.md.
 *
 *  Watches SuperServer's thread-per-attachment topology from the outside:
 *
 *    - MON$SERVER_PID names the engine process; /proc/<pid>/task counts its
 *      threads, so the sample measures the thread census before, during and
 *      after opening twelve concurrent attachments from twelve client threads.
 *    - MON$ATTACHMENTS shows the census's background workers as system
 *      attachments (MON$SYSTEM_FLAG = 1): Cache Writer, Garbage Collector.
 *    - Every attachment reports the same MON$SERVER_PID — one process, many
 *      threads: ServerMode = Super in action.
 */

#include "fb_sample.h"
#include <dirent.h>
#include <thread>
#include <chrono>

using namespace fbsample;

static const char* DEFAULT_DB = "inet://localhost//tmp/fbhandson/threading.fdb";

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

int main(int argc, char** argv)
{
	const char* dbName = argOrDefault(argc, argv, 1, DEFAULT_DB);
	try
	{
		Db db;
		db.attachOrCreate(dbName);
		ITransaction* t = db.start();
		try { db.exec(t, "recreate table t (id int primary key, v int)"); }
		catch (const FbException&) {}
		t->commit(&db.status);
		t = db.start();
		db.exec(t, "update or insert into t values (1, 0) matching (id)");
		t->commit(&db.status);

		t = db.start();
		std::string pid = db.queryValue(t,
			"select MON$SERVER_PID from MON$ATTACHMENTS "
			"where MON$ATTACHMENT_ID = current_connection");
		t->commit(&db.status);
		printf("engine process: pid %s, %d threads (1 attachment open)\n",
			pid.c_str(), countThreads(pid));

		// Twelve attachments from twelve threads of THIS process; each holds
		// its attachment open for 2 s.  Server side: one thread each (drawn
		// from the pool when idle threads exist, created otherwise).
		std::vector<std::thread> workers;
		for (int i = 0; i < 12; ++i)
			workers.emplace_back([&, i]() {
				Db w;
				w.attach(dbName);
				ITransaction* wt = w.start();
				w.queryValue(wt, "select count(*) from t");
				wt->commit(&w.status);
				std::this_thread::sleep_for(std::chrono::seconds(2));
			});

		std::this_thread::sleep_for(std::chrono::seconds(1));
		t = db.start();
		std::string users = db.queryValue(t,
			"select count(*) from MON$ATTACHMENTS where MON$SYSTEM_FLAG = 0");
		std::string pids = db.queryValue(t,
			"select count(distinct MON$SERVER_PID) from MON$ATTACHMENTS");
		t->commit(&db.status);
		printf("with 12 extra attachments: %d threads | %s user attachments, "
			"%s distinct server pid\n", countThreads(pid), users.c_str(), pids.c_str());

		for (auto& w : workers)
			w.join();
		std::this_thread::sleep_for(std::chrono::seconds(1));
		printf("after they detach:        %d threads (pooled, not destroyed)\n",
			countThreads(pid));

		// The engine's own workers hold real attachments, visible from SQL.
		t = db.start();
		Db::Table sys = db.query(t,
			"select MON$ATTACHMENT_ID, MON$SYSTEM_FLAG, trim(MON$USER), "
			"       coalesce(MON$REMOTE_PROCESS, '<internal>') "
			"from MON$ATTACHMENTS order by MON$ATTACHMENT_ID");
		Db::print(sys);
		t->commit(&db.status);
		return 0;
	}
	catch (const FbException& e) { return report(e); }
}
