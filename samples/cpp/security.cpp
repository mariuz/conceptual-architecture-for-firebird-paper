/*
 *  security.cpp — the security layers observed from client code.
 *
 *  Companion to ../../security-architecture.md.  In one run:
 *    1. WHO AM I    — the attachment's own MON$ATTACHMENTS row: auth plugin
 *                     (Srp256) and wire-crypt plugin (ChaCha64), i.e. layers
 *                     1 and 2 as the server recorded them.
 *    2. USERS       — SEC$USERS, the virtual view over the security database.
 *    3. LEAST PRIVILEGE — a temporary user plus a role carrying the
 *                     MONITOR_ANY_ATTACHMENT system privilege; connecting
 *                     with and without the role changes how much of
 *                     MON$ATTACHMENTS the same user can see.
 *    4. FAILED LOGIN — the error chain a wrong password produces.
 *  All DDL happens on a scratch database / the security database; the demo
 *  cleans up its user and role at the end.
 */

#include "fb_sample.h"

using namespace Firebird;

static const char* DSN = "inet://localhost//tmp/fbhandson/security.fdb";
static const char* TMP_USER = "HANDSON_USER";
static const char* TMP_PASS = "Hands0nPw";

// Run one statement in its own transaction, ignoring any error — needed for
// idempotent cleanup, because DROP USER reports "record not found" only at
// COMMIT (user management is deferred work, executed at commit time).
static void execIgnore(fbsample::Db& db, const char* sql)
{
	ITransaction* t = nullptr;
	try
	{
		t = db.start();
		db.exec(t, sql);
		t->commit(&db.status);
	}
	catch (const FbException&)
	{
		if (t)
		{
			CheckStatusWrapper quiet(fbsample::master->getStatus());
			t->rollback(&quiet);
			quiet.dispose();
		}
	}
}

static void whoAmI(fbsample::Db& db, ITransaction* tra, const char* label)
{
	fbsample::Db::Table t = db.query(tra,
		"select trim(mon$user), mon$auth_method, mon$wire_crypt_plugin, mon$remote_protocol,"
		"       trim(coalesce(current_role, 'NONE'))"
		"  from mon$attachments where mon$attachment_id = current_connection");
	printf("%-22s user=%s auth=%s wirecrypt=%s protocol=%s role=%s\n", label,
		t.rows[0][0].c_str(), t.rows[0][1].c_str(), t.rows[0][2].c_str(),
		t.rows[0][3].c_str(), t.rows[0][4].c_str());
}

static std::string visibleAttachments(fbsample::Db& db, ITransaction* tra)
{
	return db.queryValue(tra,
		"select count(*) from mon$attachments where mon$system_flag = 0");
}

int main()
{
	try
	{
		fbsample::Db admin;
		admin.attachOrCreate(DSN);

		// 1. Layers 1+2, as recorded for THIS attachment.
		{
			ITransaction* t = admin.start();
			whoAmI(admin, t, "admin attachment:");
			t->commit(&admin.status);
		}

		// 2+3. A temporary user (security database) and a privileged role
		//      (this database).  Cleanup first, in case a prior run died.
		//      Every DDL batch gets its own transaction and a FULL commit:
		//      user management is deferred work performed at commit, and on
		//      this Firebird 6 snapshot continuing a transaction after a
		//      COMMIT RETAINING of CREATE USER kills the connection.
		execIgnore(admin, "drop user HANDSON_USER using plugin Srp");
		execIgnore(admin, "drop role HANDSON_MONITOR");
		{
			ITransaction* t = admin.start();
			admin.exec(t, "create user HANDSON_USER password 'Hands0nPw' using plugin Srp");
			t->commit(&admin.status);          // <- the user exists only now
		}
		{
			ITransaction* t = admin.start();
			admin.exec(t,
				"create role HANDSON_MONITOR set system privileges to MONITOR_ANY_ATTACHMENT");
			admin.exec(t, "grant HANDSON_MONITOR to user HANDSON_USER");
			t->commit(&admin.status);
		}

		ITransaction* tra = admin.start();
		printf("\nSEC$USERS (the security database, through the virtual view):\n");
		fbsample::Db::Table users = admin.query(tra,
			"select trim(sec$user_name), trim(sec$plugin), sec$admin"
			"  from sec$users order by 1");
		printf("    %-16s %-8s %s\n", "USER", "PLUGIN", "ADMIN");
		for (const auto& r : users.rows)
			printf("    %-16s %-8s %s\n", r[0].c_str(), r[1].c_str(), r[2].c_str());

		// 3. Same user, without and with the role: the system privilege
		//    decides how much of MON$ is visible.  (admin stays attached,
		//    so a fully-privileged viewer sees at least 2 attachments.)
		printf("\nadmin sees %s user attachments in MON$ATTACHMENTS\n",
			visibleAttachments(admin, tra).c_str());

		{
			fbsample::Db plain;
			plain.attach(DSN, TMP_USER, TMP_PASS);
			ITransaction* t2 = plain.start();
			whoAmI(plain, t2, "user, no role:");
			printf("  -> sees %s attachment(s): only its own\n",
				visibleAttachments(plain, t2).c_str());
			t2->commit(&plain.status);
		}
		{
			fbsample::Db monitor;    // attach WITH the role: DPB + sql_role_name
			IXpbBuilder* dpb = monitor.makeDpb(TMP_USER, TMP_PASS, "UTF8");
			dpb->insertString(&monitor.status, isc_dpb_sql_role_name, "HANDSON_MONITOR");
			monitor.att = monitor.prov->attachDatabase(&monitor.status, DSN,
				dpb->getBufferLength(&monitor.status), dpb->getBuffer(&monitor.status));
			dpb->dispose();
			ITransaction* t3 = monitor.start();
			whoAmI(monitor, t3, "user + role:");
			printf("  -> sees %s attachments: MONITOR_ANY_ATTACHMENT at work\n",
				visibleAttachments(monitor, t3).c_str());
			t3->commit(&monitor.status);
		}

		// 4. The failed login, and its exact error chain.
		printf("\nfailed login (wrong password) produces:\n");
		try
		{
			fbsample::Db bad;
			bad.attach(DSN, TMP_USER, "wrong-password");
		}
		catch (const FbException& e)
		{
			char buf[1024];
			fbsample::master->getUtilInterface()->formatStatus(buf, sizeof(buf), e.getStatus());
			printf("    %s\n", buf);
		}

		tra->commit(&admin.status);

		// Cleanup, again one transaction per DDL batch with a full commit.
		{
			ITransaction* t = admin.start();
			admin.exec(t, "drop user HANDSON_USER using plugin Srp");
			admin.exec(t, "drop role HANDSON_MONITOR");
			t->commit(&admin.status);
		}
		printf("\ntemporary user and role dropped. done.\n");
	}
	catch (const FbException& e)
	{
		return fbsample::report(e);
	}
	return 0;
}
