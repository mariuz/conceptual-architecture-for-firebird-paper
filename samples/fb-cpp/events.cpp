/*
 *  events.cpp (fb-cpp) — commit-time delivery, rollback, and coalescing,
 *  through fb-cpp's EventListener.
 *
 *  The fb-cpp twin of ../events_demo.cpp.  The OO-API version hand-rolls the
 *  whole client side: isc_event_block for the EPB, an IEventCallback
 *  implementation with manual addRef/release, isc_event_counts for the
 *  delta, and an explicit re-queue after every delivery (events are
 *  one-shot).  EventListener does all of that internally — it consumes the
 *  baseline delivery, computes per-name deltas, re-queues itself, and hands
 *  the callback a vector of {name, count} on a dispatcher thread.  The three
 *  engine semantics being demonstrated are unchanged: rollback swallows
 *  posts, delivery waits for commit, posts coalesce to one delivery with a
 *  count.  See ../../firebird-events.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_events [database]
 */

#include "fbcpp_sample.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

using namespace fbcpp;
using namespace fbcpp_sample;

// Collects counts delivered by the EventListener's dispatcher thread.
class Delivered
{
public:
	void add(unsigned n)
	{
		std::lock_guard<std::mutex> g{m};
		count += n;
		cv.notify_all();
	}

	// Wait for a delivery (or timeout) and return the coalesced count.
	unsigned take(unsigned timeoutMs)
	{
		std::unique_lock<std::mutex> l{m};
		cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [this] { return count != 0; });
		const unsigned got = count;
		count = 0;
		return got;
	}

private:
	std::mutex m;
	std::condition_variable cv;
	unsigned count = 0;
};

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1,
		"inet://localhost//tmp/fbhandson/events_fbcpp.fdb");
	int rc = 0;

	try
	{
		Client client{"fbclient"};

		// Two attachments: a listener and a poster.
		Attachment listenAtt = attachOrCreate(client, database);
		Attachment postAtt{client, database, defaultOptions()};

		Delivered delivered;
		EventListener listener{listenAtt, {"demo_event"},
			[&delivered](const std::vector<EventCount>& counts)
			{
				for (const auto& c : counts)
					delivered.add(c.count);
			}};
		printf("listener registered for 'demo_event' (baseline consumed by EventListener)\n");

		// 1. POST_EVENT then ROLLBACK: nothing may be delivered.
		{
			Transaction tra{postAtt};
			postAtt.execute(tra, "execute block as begin post_event 'demo_event'; end");
			tra.rollback();

			const unsigned got = delivered.take(1500);
			printf("after POST_EVENT + ROLLBACK: delivered count = %u  %s\n", got,
				got == 0 ? "(correct - rollback swallows posts)" : "(UNEXPECTED)");
			if (got != 0)
				rc = 1;
		}

		// 2. Three POST_EVENTs in one transaction, then COMMIT:
		//    one delivery with count 3.
		{
			Transaction tra{postAtt};
			postAtt.execute(tra, "execute block as begin"
				" post_event 'demo_event'; post_event 'demo_event'; post_event 'demo_event'; end");
			printf("3 x POST_EVENT executed, not yet committed - waiting briefly...\n");

			const unsigned early = delivered.take(1000);
			printf("before COMMIT: delivered count = %u  %s\n", early,
				early == 0 ? "(correct - delivery is commit-time)" : "(UNEXPECTED)");
			if (early != 0)
				rc = 1;

			tra.commit();
			const unsigned got = delivered.take(3000);
			printf("after COMMIT: delivered count = %u  %s\n", got,
				got == 3 ? "(correct - one delivery, count 3)" : "(UNEXPECTED)");
			if (got != 3)
				rc = 1;
		}

		listener.stop();
		printf(rc == 0 ? "PASS\n" : "FAIL\n");
		return rc;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
