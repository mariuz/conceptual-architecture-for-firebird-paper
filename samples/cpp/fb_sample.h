/*
 *  fb_sample.h — shared boilerplate for the per-document hands-on samples.
 *
 *  Every sample in this directory demonstrates one companion document of the
 *  paper (see ../../README.md).  They all need the same skeleton — obtain the
 *  master interface, build a DPB, attach, run SQL, print rows, report errors —
 *  so that skeleton lives here and each sample stays focused on its topic.
 *
 *  Uses only the public OO API from firebird/Interface.h (Firebird 3+ / 6).
 *  See ../client_test.cpp for the same steps written out longhand.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <firebird/Interface.h>

namespace fbsample
{

using namespace Firebird;

// The master interface is the single entry point of the API; it is obtained
// through the only plain C function the client library exports.
inline IMaster* master = fb_get_master_interface();

// One attachment plus the interfaces needed to talk to it.  RAII: the
// destructor releases whatever an exception left behind.
class Db
{
public:
	ThrowStatusWrapper status;
	IProvider* prov;
	IUtil* util;
	IAttachment* att = nullptr;

	Db()
		: status(master->getStatus()),
		  prov(master->getDispatcher()),
		  util(master->getUtilInterface())
	{}

	Db(const Db&) = delete;
	Db& operator=(const Db&) = delete;

	~Db()
	{
		// Best-effort cleanup; errors during unwind are ignored.
		if (att)
		{
			CheckStatusWrapper quiet(master->getStatus());
			att->detach(&quiet);
			if (quiet.getState() & IStatus::STATE_ERRORS)
				att->release();
			quiet.dispose();
		}
		prov->release();
		status.dispose();
	}

	// Build the standard DPB: credentials plus an optional lc_ctype.
	IXpbBuilder* makeDpb(const char* user, const char* password, const char* charset)
	{
		IXpbBuilder* dpb = util->getXpbBuilder(&status, IXpbBuilder::DPB, nullptr, 0);
		dpb->insertString(&status, isc_dpb_user_name, user);
		dpb->insertString(&status, isc_dpb_password, password);
		if (charset && *charset)
			dpb->insertString(&status, isc_dpb_lc_ctype, charset);
		return dpb;
	}

	// Attach to an existing database.
	void attach(const char* database,
		const char* user = defaultUser(), const char* password = defaultPassword(),
		const char* charset = "UTF8")
	{
		IXpbBuilder* dpb = makeDpb(user, password, charset);
		att = prov->attachDatabase(&status, database,
			dpb->getBufferLength(&status), dpb->getBuffer(&status));
		dpb->dispose();
	}

	// Attach if the database exists, create it otherwise (page size 8K, UTF8).
	void attachOrCreate(const char* database,
		const char* user = defaultUser(), const char* password = defaultPassword())
	{
		IXpbBuilder* dpb = makeDpb(user, password, "UTF8");
		try
		{
			att = prov->attachDatabase(&status, database,
				dpb->getBufferLength(&status), dpb->getBuffer(&status));
		}
		catch (const FbException&)
		{
			dpb->insertInt(&status, isc_dpb_page_size, 8 * 1024);
			dpb->insertString(&status, isc_dpb_set_db_charset, "UTF8");
			att = prov->createDatabase(&status, database,
				dpb->getBufferLength(&status), dpb->getBuffer(&status));
		}
		dpb->dispose();
	}

	// Start a transaction.  With no arguments you get the engine default
	// (SNAPSHOT WAIT); pass TPB bytes to choose isolation explicitly — see
	// tpb() below and ../../transactions-and-concurrency.md.
	ITransaction* start(const std::vector<unsigned char>& tpb = {})
	{
		return att->startTransaction(&status,
			static_cast<unsigned>(tpb.size()), tpb.empty() ? nullptr : tpb.data());
	}

	// Execute a statement with no result set (DDL, INSERT, ...).
	void exec(ITransaction* tra, const std::string& sql)
	{
		att->execute(&status, tra, 0, sql.c_str(), SQL_DIALECT_V6,
			nullptr, nullptr, nullptr, nullptr);
	}

	// A fetched result set, every value rendered as text.
	struct Table
	{
		std::vector<std::string> names;
		std::vector<std::vector<std::string>> rows;   // "<null>" for SQL NULL
	};

	// Run a SELECT and fetch everything as strings: output metadata is
	// coerced column-by-column to VARCHAR, delegating the formatting of
	// numbers, dates, booleans etc. to the engine's own conversion rules
	// (the same CVT machinery isql relies on).
	Table query(ITransaction* tra, const std::string& sql)
	{
		Table out;

		IStatement* stmt = att->prepare(&status, tra, 0, sql.c_str(),
			SQL_DIALECT_V6, IStatement::PREPARE_PREFETCH_METADATA);

		IMessageMetadata* meta = stmt->getOutputMetadata(&status);
		IMetadataBuilder* builder = meta->getBuilder(&status);
		const unsigned cols = meta->getCount(&status);
		for (unsigned i = 0; i < cols; ++i)
		{
			out.names.emplace_back(meta->getField(&status, i));
			builder->setType(&status, i, SQL_VARYING);
			builder->setLength(&status, i, 512);
			builder->setCharSet(&status, i, 0);	// CS_NONE: bytes as sent
		}
		meta->release();
		meta = builder->getMetadata(&status);
		builder->release();

		std::vector<unsigned char> buffer(meta->getMessageLength(&status));
		IResultSet* curs = stmt->openCursor(&status, tra, nullptr, nullptr, meta, 0);

		while (curs->fetchNext(&status, buffer.data()) == IStatus::RESULT_OK)
		{
			std::vector<std::string> row;
			for (unsigned i = 0; i < cols; ++i)
			{
				const unsigned char* p = buffer.data() + meta->getOffset(&status, i);
				const short* nullFlag = reinterpret_cast<const short*>(
					buffer.data() + meta->getNullOffset(&status, i));
				if (*nullFlag)
					row.emplace_back("<null>");
				else
				{
					// VARCHAR: 2-byte length prefix, then the bytes.
					unsigned short len;
					memcpy(&len, p, sizeof len);
					row.emplace_back(reinterpret_cast<const char*>(p + 2), len);
				}
			}
			out.rows.push_back(std::move(row));
		}

		curs->close(&status);
		meta->release();
		stmt->free(&status);
		return out;
	}

	// Convenience: run a single-value query and return the value as text.
	std::string queryValue(ITransaction* tra, const std::string& sql)
	{
		Table t = query(tra, sql);
		return t.rows.empty() ? std::string("<no rows>") : t.rows[0][0];
	}

	// Print a Table the way isql would.
	static void print(const Table& t)
	{
		std::vector<size_t> w;
		for (size_t c = 0; c < t.names.size(); ++c)
		{
			size_t m = t.names[c].size();
			for (auto& r : t.rows)
				m = std::max(m, r[c].size());
			w.push_back(m);
		}
		for (size_t c = 0; c < t.names.size(); ++c)
			printf("%-*s ", static_cast<int>(w[c]), t.names[c].c_str());
		printf("\n");
		for (size_t c = 0; c < t.names.size(); ++c)
			printf("%s ", std::string(w[c], '-').c_str());
		printf("\n");
		for (auto& r : t.rows)
		{
			for (size_t c = 0; c < r.size(); ++c)
				printf("%-*s ", static_cast<int>(w[c]), r[c].c_str());
			printf("\n");
		}
	}

	static const char* defaultUser()
	{
		const char* u = getenv("ISC_USER");
		return u ? u : "SYSDBA";
	}

	static const char* defaultPassword()
	{
		const char* p = getenv("ISC_PASSWORD");
		return p ? p : "masterkey";
	}
};

// Build a TPB from isc_tpb_* constants, e.g.
//   db.start(fbsample::tpb({isc_tpb_read_committed, isc_tpb_rec_version,
//                           isc_tpb_nowait}))
inline std::vector<unsigned char> tpb(std::initializer_list<unsigned char> items)
{
	std::vector<unsigned char> v{isc_tpb_version3};
	v.insert(v.end(), items.begin(), items.end());
	return v;
}

// Format and print an FbException, return an exit code — so every sample's
// main() can end with  } catch (...) { return fbsample::report(...); }
inline int report(const FbException& error)
{
	char buf[1024];
	master->getUtilInterface()->formatStatus(buf, sizeof(buf), error.getStatus());
	fprintf(stderr, "Firebird error:\n%s\n", buf);
	return 1;
}

// Default connection string for the samples: the live demo server used
// throughout the companion documents, overridable per run.
inline const char* argOrDefault(int argc, char** argv, int n, const char* dflt)
{
	return argc > n ? argv[n] : dflt;
}

} // namespace fbsample
