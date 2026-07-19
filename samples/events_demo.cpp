/*
 * events_demo.cpp — Firebird event-notification demo for the events companion
 * document (../firebird-events.md).
 *
 * Two attachments to the same database over TCP:
 *   - a LISTENER registers interest in the event 'demo_event' with
 *     IAttachment::queEvents() (this opens the auxiliary connection and
 *     sends op_que_events);
 *   - a POSTER executes PSQL blocks containing POST_EVENT.
 *
 * The demo shows the three defining semantics of Firebird events:
 *   1. delivery happens at COMMIT, not when POST_EVENT executes;
 *   2. a ROLLBACK discards pending posts — nothing is delivered;
 *   3. multiple posts of the same name in one transaction are delivered
 *      once, with a count (isc_event_counts computes the delta).
 *
 * Build: see CMakeLists.txt (add_executable events_demo).
 * Run:   ISC_USER=SYSDBA ISC_PASSWORD=masterkey ./events_demo [database]
 *        (database defaults to inet://localhost/employee)
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "firebird/Interface.h"
#include "ibase.h"

using namespace Firebird;

static IMaster* master = fb_get_master_interface();

// One registered event name: buffer bookkeeping + the wire callback.
// eventCallbackFunction runs on the client's event thread when an
// op_event packet arrives on the auxiliary connection.
class Listener : public IEventCallbackImpl<Listener, ThrowStatusWrapper>
{
public:
	Listener(IAttachment* att, const char* name)
		: refCounter(0), attachment(att), status(master->getStatus()),
		  events(nullptr), fired(0)
	{
		eveLen = isc_event_block(&eveBuffer, &eveResult, 1, name);
		requeue();
	}

	// (Re-)register interest. Events are one-shot: after every delivery the
	// interest must be queued again — with the previous result buffer as the
	// new baseline, so only *new* posts count.
	void requeue()
	{
		if (events)
		{
			events->release();
			events = nullptr;
		}
		events = attachment->queEvents(&status, this, eveLen, eveBuffer);
	}

	// Wait until the callback has fired, or timeout. Returns delivered count.
	ISC_ULONG wait(unsigned timeoutMs)
	{
		for (unsigned waited = 0; !fired && waited < timeoutMs; waited += 50)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));

		if (!fired)
			return 0;

		ISC_ULONG total = 0;
		isc_event_counts(&total, eveLen, eveBuffer, eveResult);
		fired = 0;
		return total;
	}

	void shutdown()
	{
		if (events)
		{
			events->cancel(&status);
			events = nullptr;
		}
	}

	// IEventCallback
	void eventCallbackFunction(unsigned int length, const ISC_UCHAR* data) override
	{
		if (length <= eveLen)
			memcpy(eveResult, data, length);
		++fired;
	}

	void addRef() override { ++refCounter; }

	int release() override
	{
		if (--refCounter == 0)
		{
			delete this;
			return 0;
		}
		return 1;
	}

private:
	~Listener()
	{
		if (eveBuffer)
			isc_free((char*) eveBuffer);
		if (eveResult)
			isc_free((char*) eveResult);
		status.dispose();
	}

	std::atomic_int refCounter;
	IAttachment* attachment;
	ThrowStatusWrapper status;
	IEvents* events;
	std::atomic_int fired;
	ISC_UCHAR* eveBuffer = nullptr;
	ISC_UCHAR* eveResult = nullptr;
	unsigned eveLen = 0;
};

static void runBlock(ThrowStatusWrapper& status, IAttachment* att, ITransaction* tra,
	const char* sql)
{
	att->execute(&status, tra, 0, sql, SQL_DIALECT_CURRENT,
		nullptr, nullptr, nullptr, nullptr);
}

int main(int argc, char** argv)
{
	const char* database = argc > 1 ? argv[1] : "inet://localhost/employee";

	setenv("ISC_USER", "SYSDBA", 0);
	setenv("ISC_PASSWORD", "masterkey", 0);

	ThrowStatusWrapper status(master->getStatus());
	IProvider* prov = master->getDispatcher();
	IUtil* utl = master->getUtilInterface();

	IAttachment* listenAtt = nullptr;
	IAttachment* postAtt = nullptr;
	Listener* listener = nullptr;
	int rc = 0;

	try
	{
		IXpbBuilder* dpb = utl->getXpbBuilder(&status, IXpbBuilder::DPB, nullptr, 0);
		dpb->insertString(&status, isc_dpb_user_name, getenv("ISC_USER"));
		dpb->insertString(&status, isc_dpb_password, getenv("ISC_PASSWORD"));

		listenAtt = prov->attachDatabase(&status, database,
			dpb->getBufferLength(&status), dpb->getBuffer(&status));
		postAtt = prov->attachDatabase(&status, database,
			dpb->getBufferLength(&status), dpb->getBuffer(&status));
		dpb->dispose();

		// Register interest. The first delivery comes immediately — it
		// carries the current counts as a baseline; consume and re-queue.
		listener = new Listener(listenAtt, "demo_event");
		listener->addRef();
		listener->wait(3000);
		listener->requeue();
		printf("listener registered for 'demo_event' (baseline consumed)\n");
		fflush(stdout);

		// Optional pause so the auxiliary (event) connection can be observed
		// from outside, e.g. in /proc/net/tcp: EVENTS_DEMO_PAUSE_MS=5000
		if (const char* pauseMs = getenv("EVENTS_DEMO_PAUSE_MS"))
			std::this_thread::sleep_for(std::chrono::milliseconds(atoi(pauseMs)));

		// 1. POST_EVENT then ROLLBACK: nothing may be delivered.
		{
			ITransaction* tra = postAtt->startTransaction(&status, 0, nullptr);
			runBlock(status, postAtt, tra, "EXECUTE BLOCK AS BEGIN POST_EVENT 'demo_event'; END");
			tra->rollback(&status);

			const ISC_ULONG got = listener->wait(1500);
			printf("after POST_EVENT + ROLLBACK: delivered count = %lu  %s\n",
				(unsigned long) got, got == 0 ? "(correct - rollback swallows posts)" : "(UNEXPECTED)");
			if (got != 0)
				rc = 1;
		}

		// 2. Three POST_EVENTs in one transaction, then COMMIT:
		//    one delivery with count 3.
		{
			ITransaction* tra = postAtt->startTransaction(&status, 0, nullptr);
			runBlock(status, postAtt, tra,
				"EXECUTE BLOCK AS BEGIN POST_EVENT 'demo_event'; POST_EVENT 'demo_event'; POST_EVENT 'demo_event'; END");
			printf("3 x POST_EVENT executed, not yet committed - waiting briefly...\n");

			ISC_ULONG early = listener->wait(1000);
			printf("before COMMIT: delivered count = %lu  %s\n", (unsigned long) early,
				early == 0 ? "(correct - delivery is commit-time)" : "(UNEXPECTED)");
			if (early != 0)
				rc = 1;

			tra->commit(&status);
			const ISC_ULONG got = listener->wait(3000);
			printf("after COMMIT: delivered count = %lu  %s\n", (unsigned long) got,
				got == 3 ? "(correct - one delivery, count 3)" : "(UNEXPECTED)");
			if (got != 3)
				rc = 1;
		}

		listener->shutdown();
		listener->release();
		listener = nullptr;

		postAtt->detach(&status);
		postAtt = nullptr;
		listenAtt->detach(&status);
		listenAtt = nullptr;

		printf(rc == 0 ? "PASS\n" : "FAIL\n");
	}
	catch (const FbException& e)
	{
		char buf[512];
		utl->formatStatus(buf, sizeof(buf), e.getStatus());
		fprintf(stderr, "error: %s\n", buf);
		rc = 1;
	}

	prov->release();
	return rc;
}
