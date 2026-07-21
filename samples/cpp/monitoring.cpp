/*
 *  monitoring.cpp — companion sample for ../../monitoring-and-tuning.md
 *
 *  Walks the MON$ hierarchy (database -> attachment -> transaction ->
 *  statement, with counters joined via MON$STAT_ID) and then demonstrates
 *  the defining architectural property of the monitoring tables: the first
 *  MON$ select in a transaction takes a STABLE SNAPSHOT.  The sample runs a
 *  full-scan workload, shows its own counters unchanged inside the same
 *  transaction, then refreshed in a new one.
 */

#include "fb_sample.h"

using namespace Firebird;

static const char* DB = "inet://localhost//tmp/fbhandson/monitoring.fdb";

// This attachment's record/page counters, via the MON$STAT_ID join.
static const char* COUNTERS =
	"SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS, R.MON$RECORD_INSERTS, "
	"       I.MON$PAGE_FETCHES, I.MON$PAGE_READS "
	"FROM MON$ATTACHMENTS A "
	"JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID "
	"JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID "
	"WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION";

int main(int argc, char** argv)
{
	try
	{
		fbsample::Db db;
		db.attachOrCreate(fbsample::argOrDefault(argc, argv, 1, DB));
		ITransaction* tra = db.start();

		// Workload table: 10000 rows to scan.
		try { db.exec(tra, "DROP TABLE MON_WORK"); } catch (const FbException&) {}
		db.exec(tra, "CREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)");
		tra->commitRetaining(&db.status);
		db.exec(tra,
			"EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
			"  WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); I = I + 1; END "
			"END");
		tra->commit(&db.status);

		// -- 1. the hierarchy, one level per query, one consistent snapshot --
		tra = db.start();
		printf("== MON$DATABASE: transaction markers ==\n");
		fbsample::Db::print(db.query(tra,
			"SELECT MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$NEXT_TRANSACTION, "
			"       MON$PAGE_BUFFERS FROM MON$DATABASE"));

		printf("\n== MON$ATTACHMENTS -> MON$TRANSACTIONS -> MON$STATEMENTS (me) ==\n");
		fbsample::Db::print(db.query(tra,
			"SELECT A.MON$ATTACHMENT_ID, A.MON$USER, T.MON$TRANSACTION_ID, "
			"       S.MON$STATE, CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 40) AS VARCHAR(40)) AS SQL_HEAD "
			"FROM MON$ATTACHMENTS A "
			"JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID "
			"JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID "
			"WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION"));

		// -- 2. the snapshot property, measured on our own counters ---------
		printf("\n== my counters (snapshot 1) ==\n");
		fbsample::Db::print(db.query(tra, COUNTERS));

		printf("\n... running workload: SELECT COUNT(*) full scan + indexed lookup ...\n");
		printf("count = %s, point = %s\n",
			db.queryValue(tra, "SELECT COUNT(*) FROM MON_WORK").c_str(),
			db.queryValue(tra, "SELECT VAL FROM MON_WORK WHERE ID = 4242").c_str());

		printf("\n== same transaction, re-queried: STILL snapshot 1 ==\n");
		fbsample::Db::print(db.query(tra, COUNTERS));
		tra->commit(&db.status);

		tra = db.start();
		printf("\n== new transaction: fresh snapshot, workload now visible ==\n");
		fbsample::Db::print(db.query(tra, COUNTERS));
		tra->commit(&db.status);

		printf("\ndone.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
}
