/*
 *  backup.cpp — companion sample for ../../backup-and-recovery.md
 *
 *  A gbak backup + restore round trip driven entirely through the Services
 *  API — the same path `gbak -se` and fbsvcmgr use, so no gbak binary is
 *  needed on the client:
 *
 *    1. create a scratch database with a table and a few rows;
 *    2. attach to service_mgr and start isc_action_svc_backup (verbose),
 *       streaming gbak's log line by line with isc_info_svc_line;
 *    3. start isc_action_svc_restore (replace) the same way;
 *    4. attach to the restored database and prove the rows survived.
 *
 *  The backup runs while the source attachment is still open: gbak reads
 *  through a snapshot transaction, the "Online" property of the document's
 *  gbak section.
 */

#include "fb_sample.h"

using namespace Firebird;
using fbsample::master;

static const char* DB_SRC  = "inet://localhost//tmp/fbhandson/backup.fdb";
static const char* SRV     = "localhost:service_mgr";
static const char* FBK     = "/tmp/fbhandson/backup.fbk";           // server-side path
static const char* DB_REST = "inet://localhost//tmp/fbhandson/backup_restored.fdb";

// Drain one service's output: query isc_info_svc_line until the line is empty.
static void streamServiceOutput(ThrowStatusWrapper& st, IService* svc, const char* pfx)
{
	const unsigned char items[] = { isc_info_svc_line };
	unsigned char buf[1024];
	for (;;)
	{
		svc->query(&st, 0, nullptr, sizeof(items), items, sizeof(buf), buf);
		if (buf[0] != isc_info_svc_line)
			break;
		const unsigned len = buf[1] | (buf[2] << 8);        // little-endian VAX word
		if (!len)
			break;                                          // empty line = service done
		printf("%s%.*s\n", pfx, len, buf + 3);
	}
}

int main(int argc, char** argv)
{
	try
	{
		// -- 1. scratch source database ---------------------------------
		fbsample::Db db;
		db.attachOrCreate(fbsample::argOrDefault(argc, argv, 1, DB_SRC));
		ITransaction* tra = db.start();
		try { db.exec(tra, "DROP TABLE BR_ITEMS"); } catch (const FbException&) {}
		db.exec(tra, "CREATE TABLE BR_ITEMS (ID INT NOT NULL PRIMARY KEY, NAME VARCHAR(30))");
		tra->commitRetaining(&db.status);
		db.exec(tra, "INSERT INTO BR_ITEMS VALUES (1, 'alpha')");
		db.exec(tra, "INSERT INTO BR_ITEMS VALUES (2, 'beta')");
		db.exec(tra, "INSERT INTO BR_ITEMS VALUES (3, 'gamma')");
		tra->commit(&db.status);
		printf("source ready: BR_ITEMS with 3 rows\n");

		// -- 2. attach to the service manager ---------------------------
		ThrowStatusWrapper st(master->getStatus());
		IProvider* prov = master->getDispatcher();
		IUtil* utl = master->getUtilInterface();

		IXpbBuilder* spb = utl->getXpbBuilder(&st, IXpbBuilder::SPB_ATTACH, nullptr, 0);
		spb->insertString(&st, isc_spb_user_name, fbsample::Db::defaultUser());
		spb->insertString(&st, isc_spb_password, fbsample::Db::defaultPassword());
		IService* svc = prov->attachServiceManager(&st, SRV,
			spb->getBufferLength(&st), spb->getBuffer(&st));
		spb->dispose();

		// -- 3. gbak backup through the service, verbose ----------------
		printf("\n== backup: %s -> %s ==\n", "/tmp/fbhandson/backup.fdb", FBK);
		IXpbBuilder* start = utl->getXpbBuilder(&st, IXpbBuilder::SPB_START, nullptr, 0);
		start->insertTag(&st, isc_action_svc_backup);
		start->insertString(&st, isc_spb_dbname, "/tmp/fbhandson/backup.fdb");
		start->insertString(&st, isc_spb_bkp_file, FBK);
		start->insertTag(&st, isc_spb_verbose);
		svc->start(&st, start->getBufferLength(&st), start->getBuffer(&st));
		start->dispose();
		streamServiceOutput(st, svc, "  gbak> ");

		// -- 4. gbak restore (replace) through the same service ---------
		printf("\n== restore: %s -> %s ==\n", FBK, "/tmp/fbhandson/backup_restored.fdb");
		start = utl->getXpbBuilder(&st, IXpbBuilder::SPB_START, nullptr, 0);
		start->insertTag(&st, isc_action_svc_restore);
		start->insertString(&st, isc_spb_bkp_file, FBK);
		start->insertString(&st, isc_spb_dbname, "/tmp/fbhandson/backup_restored.fdb");
		start->insertInt(&st, isc_spb_options, isc_spb_res_replace);
		start->insertTag(&st, isc_spb_verbose);
		svc->start(&st, start->getBufferLength(&st), start->getBuffer(&st));
		start->dispose();
		streamServiceOutput(st, svc, "  gbak> ");

		svc->detach(&st);
		prov->release();
		st.dispose();

		// -- 5. prove the restored copy has the data --------------------
		fbsample::Db rdb;
		rdb.attach(DB_REST);
		ITransaction* rtra = rdb.start();
		printf("\nrestored database says: %s rows, max name = %s\n",
			rdb.queryValue(rtra, "SELECT COUNT(*) FROM BR_ITEMS").c_str(),
			rdb.queryValue(rtra, "SELECT MAX(NAME) FROM BR_ITEMS").c_str());
		rtra->commit(&rdb.status);
		printf("done.\n");
		return 0;
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
}
