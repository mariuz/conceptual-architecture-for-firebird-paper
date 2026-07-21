/*
 *  security.cpp (fb-cpp) — the security layers observed from client code.
 *
 *  The fb-cpp twin of ../cpp/security.cpp, same four steps: the attachment's
 *  own MON$ATTACHMENTS row (auth plugin, wire-crypt plugin), SEC$USERS, a
 *  temporary user plus a role carrying MONITOR_ANY_ATTACHMENT, and a failed
 *  login.  Stylistic deltas: the role travels as AttachmentOptions::setRole
 *  instead of a hand-inserted isc_dpb_sql_role_name, and the wrong password
 *  surfaces as a typed DatabaseException whose getErrorCode() is the first
 *  gds code (335544472 = isc_login).  The engine rules are unchanged — user
 *  management is deferred work executed at COMMIT, so every DDL batch gets
 *  its own transaction and a FULL commit (on this Firebird 6 snapshot,
 *  continuing a transaction after COMMIT RETAINING of CREATE USER kills the
 *  server — see the companion document).  Distinct user/role names and a
 *  distinct scratch database keep this twin runnable next to the OO-API one.
 *  See ../../security-architecture.md.
 *
 *  Build & run (see ../README.md):
 *      ./build/fbcpp_security
 */

#include "fbcpp_sample.h"
#include <cstdio>

using namespace fbcpp;
using namespace fbcpp_sample;

static const char* DSN = "inet://localhost//tmp/fbhandson/security_fbcpp.fdb";
static const char* TMP_USER = "FBCPP_USER";
static const char* TMP_PASS = "Hands0nPw";

// Run one statement in its own transaction, ignoring any error — needed for
// idempotent cleanup (DROP USER of a missing user reports only at COMMIT).
static void execIgnore(Attachment& att, const char* sql)
{
	try
	{
		Transaction t{att};
		att.execute(t, sql);
		t.commit();
	}
	catch (const DatabaseException&)
	{
		// the Transaction destructor already rolled back
	}
}

static void whoAmI(Attachment& att, Transaction& tra, const char* label)
{
	Statement s{att, tra,
		"select trim(mon$user), mon$auth_method, mon$wire_crypt_plugin, mon$remote_protocol,"
		"       trim(coalesce(current_role, 'NONE'))"
		"  from mon$attachments where mon$attachment_id = current_connection"};
	s.execute(tra);
	printf("%-22s user=%s auth=%s wirecrypt=%s protocol=%s role=%s\n", label,
		s.getString(0).value_or("?").c_str(), s.getString(1).value_or("?").c_str(),
		s.getString(2).value_or("?").c_str(), s.getString(3).value_or("?").c_str(),
		s.getString(4).value_or("?").c_str());
}

static long long visibleAttachments(Attachment& att, Transaction& tra)
{
	return (long long) att.queryScalar<std::int64_t>(tra,
		"select count(*) from mon$attachments where mon$system_flag = 0").value_or(-1);
}

int main()
{
	try
	{
		Client client{"fbclient"};
		Attachment admin = attachOrCreate(client, DSN);

		// 1. Layers 1+2, as recorded for THIS attachment.
		{
			Transaction t{admin};
			whoAmI(admin, t, "admin attachment:");
			t.commit();
		}

		// 2+3. A temporary user (security database) and a privileged role
		//      (this database).  Cleanup first, in case a prior run died;
		//      one transaction and a full commit per DDL batch (see header).
		execIgnore(admin, "drop user FBCPP_USER using plugin Srp");
		execIgnore(admin, "drop role FBCPP_MONITOR");
		{
			Transaction t{admin};
			admin.execute(t, "create user FBCPP_USER password 'Hands0nPw' using plugin Srp");
			t.commit();                        // <- the user exists only now
		}
		{
			Transaction t{admin};
			admin.execute(t,
				"create role FBCPP_MONITOR set system privileges to MONITOR_ANY_ATTACHMENT");
			admin.execute(t, "grant FBCPP_MONITOR to user FBCPP_USER");
			t.commit();
		}

		Transaction tra{admin};
		printf("\nSEC$USERS (the security database, through the virtual view):\n");
		Statement users{admin, tra,
			"select trim(sec$user_name), trim(sec$plugin), sec$admin"
			"  from sec$users order by 1"};
		printf("    %-16s %-8s %s\n", "USER", "PLUGIN", "ADMIN");
		for (bool row = users.execute(tra); row; row = users.fetchNext())
			printf("    %-16s %-8s %s\n",
				users.getString(0).value_or("?").c_str(),
				users.getString(1).value_or("?").c_str(),
				users.getBool(2).value_or(false) ? "TRUE" : "FALSE");

		// 3. Same user, without and with the role: the system privilege
		//    decides how much of MON$ is visible.
		printf("\nadmin sees %lld user attachments in MON$ATTACHMENTS\n",
			visibleAttachments(admin, tra));

		{
			Attachment plain{client, DSN, AttachmentOptions()
				.setUserName(TMP_USER).setPassword(TMP_PASS)};
			Transaction t2{plain};
			whoAmI(plain, t2, "user, no role:");
			printf("  -> sees %lld attachment(s): only its own\n",
				visibleAttachments(plain, t2));
			t2.commit();
		}
		{
			Attachment monitor{client, DSN, AttachmentOptions()
				.setUserName(TMP_USER).setPassword(TMP_PASS)
				.setRole("FBCPP_MONITOR")};   // vs. isc_dpb_sql_role_name by hand
			Transaction t3{monitor};
			whoAmI(monitor, t3, "user + role:");
			printf("  -> sees %lld attachments: MONITOR_ANY_ATTACHMENT at work\n",
				visibleAttachments(monitor, t3));
			t3.commit();
		}

		// 4. The failed login, as a typed exception.
		printf("\nfailed login (wrong password) produces:\n");
		try
		{
			Attachment bad{client, DSN, AttachmentOptions()
				.setUserName(TMP_USER).setPassword("wrong-password")};
		}
		catch (const DatabaseException& e)
		{
			printf("    gds %ld (isc_login):\n    %s\n",
				(long) e.getErrorCode(), e.what());
		}

		tra.commit();

		// Cleanup, again one transaction per DDL batch with a full commit.
		{
			Transaction t{admin};
			admin.execute(t, "drop user FBCPP_USER using plugin Srp");
			admin.execute(t, "drop role FBCPP_MONITOR");
			t.commit();
		}
		printf("\ntemporary user and role dropped. done.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		return report(e);
	}
}
