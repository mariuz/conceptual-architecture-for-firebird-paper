/*
 *  monitoring.cpp (fb-cpp) — the MON$ snapshot, typed edition.
 *
 *  The fb-cpp twin of ../cpp/monitoring.cpp: the same MON$ hierarchy walk
 *  (database -> attachment -> transaction -> statement, counters joined via
 *  MON$STAT_ID) and the same stable-snapshot demonstration — counters
 *  frozen inside one transaction, refreshed in the next.  The BIGINT
 *  counters come back through getInt64() as std::optional instead of
 *  VARCHAR coercion.  See ../../monitoring-and-tuning.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_monitoring [database]
 */

#include "fbcpp_sample.h"
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DB = "inet://localhost//tmp/fbhandson/monitoring_fbcpp.fdb";

// This attachment's record/page counters, via the MON$STAT_ID join.
static const char* COUNTERS =
	"SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS, R.MON$RECORD_INSERTS, "
	"       I.MON$PAGE_FETCHES, I.MON$PAGE_READS "
	"FROM MON$ATTACHMENTS A "
	"JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID "
	"JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID "
	"WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION";

static void showCounters(Attachment& att, Transaction& tra)
{
	Statement stmt{att, tra, COUNTERS};
	stmt.execute(tra);
	printf("seq_reads %lld   idx_reads %lld   inserts %lld   fetches %lld   reads %lld\n",
		static_cast<long long>(stmt.getInt64(0).value_or(-1)),
		static_cast<long long>(stmt.getInt64(1).value_or(-1)),
		static_cast<long long>(stmt.getInt64(2).value_or(-1)),
		static_cast<long long>(stmt.getInt64(3).value_or(-1)),
		static_cast<long long>(stmt.getInt64(4).value_or(-1)));
}

int main(int argc, char** argv)
{
	const char* database = argOrDefault(argc, argv, 1, DB);

	try
	{
		Client client{"fbclient"};
		Attachment att = attachOrCreate(client, database);

		// Workload table: 10000 rows to scan.
		{
			Transaction tra{att};
			try { Statement{att, tra, "DROP TABLE MON_WORK"}.execute(tra); }
			catch (const DatabaseException&) {}
			Statement{att, tra, "CREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)"}
				.execute(tra);
			tra.commitRetaining();
			Statement{att, tra,
				"EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
				"  WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); I = I + 1; END "
				"END"}.execute(tra);
			tra.commit();
		}

		// -- 1. the hierarchy, one level per query, one consistent snapshot --
		Transaction tra{att};
		printf("== MON$DATABASE: transaction markers ==\n");
		Statement markers{att, tra,
			"SELECT MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$NEXT_TRANSACTION, "
			"       MON$PAGE_BUFFERS FROM MON$DATABASE"};
		markers.execute(tra);
		printf("OIT %s   OAT %s   NEXT %s   page buffers %s\n",
			markers.getString(0).value_or("?").c_str(),
			markers.getString(1).value_or("?").c_str(),
			markers.getString(2).value_or("?").c_str(),
			markers.getString(3).value_or("?").c_str());

		printf("\n== MON$ATTACHMENTS -> MON$TRANSACTIONS -> MON$STATEMENTS (me) ==\n");
		Statement chain{att, tra,
			"SELECT A.MON$ATTACHMENT_ID, A.MON$USER, T.MON$TRANSACTION_ID, "
			"       S.MON$STATE, CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 40) AS VARCHAR(40)) AS SQL_HEAD "
			"FROM MON$ATTACHMENTS A "
			"JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID "
			"JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID "
			"WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION"};
		for (bool ok = chain.execute(tra); ok; ok = chain.fetchNext())
		{
			printf("att %s   user %s   tx %s   state %s   sql: %s\n",
				chain.getString(0).value_or("?").c_str(),
				chain.getString(1).value_or("?").c_str(),
				chain.getString(2).value_or("?").c_str(),
				chain.getString(3).value_or("?").c_str(),
				chain.getString(4).value_or("?").c_str());
		}

		// -- 2. the snapshot property, measured on our own counters ---------
		printf("\n== my counters (snapshot 1) ==\n");
		showCounters(att, tra);

		printf("\n... running workload: SELECT COUNT(*) full scan + indexed lookup ...\n");
		Statement scan{att, tra, "SELECT COUNT(*) FROM MON_WORK"};
		scan.execute(tra);
		Statement point{att, tra, "SELECT VAL FROM MON_WORK WHERE ID = 4242"};
		point.execute(tra);
		printf("count = %s, point = %d\n",
			scan.getString(0).value_or("?").c_str(),
			point.getInt32(0).value_or(-1));

		printf("\n== same transaction, re-queried: STILL snapshot 1 ==\n");
		showCounters(att, tra);
		tra.commit();

		Transaction tra2{att};
		printf("\n== new transaction: fresh snapshot, workload now visible ==\n");
		showCounters(att, tra2);
		tra2.commit();

		printf("\ndone.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
