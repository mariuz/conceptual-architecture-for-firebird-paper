// Package fbsample is the shared boilerplate for the Go hands-on twins.
//
// Every sample demonstrates one companion document of the paper.  They use
// firebirdsql (https://github.com/nakagami/firebirdsql), a pure-Go
// implementation of the Firebird wire protocol behind database/sql: no
// libfbclient is involved, so - like node-firebird and rsfbclient's pure
// Rust backend - it is an independent client of the protocol described in
// ../../firebird-wire-protocol.md (Srp256, ChaCha64 / Arc4 wire crypt,
// protocols 10-19).  Beyond database/sql it carries its own Services
// managers (backup, maintenance, nbackup, trace, users), events and a few
// raw-connection extras reached through sql.Conn.Raw.
//
// Like the other twins, everything runs against the local server with
// scratch databases under /tmp/fbhandson (SYSDBA/masterkey, overridable via
// ISC_USER / ISC_PASSWORD; FB_HOST picks another server).
package fbsample

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"os"
	"strings"

	"github.com/nakagami/firebirdsql"
)

// Scratch is the directory the samples' scratch databases live in.
const Scratch = "/tmp/fbhandson"

func env(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

var (
	Host     = env("FB_HOST", "localhost")
	User     = env("ISC_USER", "SYSDBA")
	Password = env("ISC_PASSWORD", "masterkey")
)

// DBPath is the server path of a topic's scratch database; the first
// command-line argument overrides it.
func DBPath(topic string) string {
	if len(os.Args) > 1 {
		return os.Args[1]
	}
	return fmt.Sprintf("%s/%s_go.fdb", Scratch, topic)
}

// DSN builds the driver's connection string for a server path or alias;
// params are extra query parameters ("charset=UTF8", "wire_compress=true").
func DSN(path string, params ...string) string {
	dsn := fmt.Sprintf("%s:%s@%s/%s", url.QueryEscape(User), url.QueryEscape(Password),
		Host, strings.TrimPrefix(path, "/"))
	if strings.HasPrefix(path, "/") {
		dsn = fmt.Sprintf("%s:%s@%s//%s", url.QueryEscape(User), url.QueryEscape(Password),
			Host, strings.TrimPrefix(path, "/"))
	}
	if len(params) > 0 {
		dsn += "?" + strings.Join(params, "&")
	}
	return dsn
}

func open(driverName, path string, params []string) (*sql.DB, error) {
	if len(params) == 0 {
		params = []string{"charset=UTF8"}
	}
	db, err := sql.Open(driverName, DSN(path, params...))
	if err != nil {
		return nil, err
	}
	// One *sql.DB = one attachment: database/sql would otherwise pool
	// connections and spread a sample's statements over several of them.
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

// Attach opens one attachment to an existing database.
func Attach(path string, params ...string) (*sql.DB, error) {
	return open("firebirdsql", path, params)
}

// Create creates the database and attaches to it.  It goes through the
// driver's "firebirdsql_createdb" name, whose op_create always carries
// isc_dpb_overwrite: an existing file at path is REPLACED, so every run of
// a sample starts on a fresh scratch database.
func Create(path string, params ...string) (*sql.DB, error) {
	return open("firebirdsql_createdb", path, params)
}

// Recreate gives a fresh scratch database: DROP DATABASE it if it exists (a
// clean detach of the old file's attachments), then Create it.
func Recreate(path string, params ...string) (*sql.DB, error) {
	if db, err := Attach(path, params...); err == nil {
		conn, err := db.Conn(context.Background())
		if err == nil {
			conn.ExecContext(context.Background(), "DROP DATABASE")
			conn.Close()
		}
		db.Close()
	}
	return Create(path, params...)
}

// Employee attaches to the demo server's employee database.
func Employee(params ...string) (*sql.DB, error) {
	if len(params) == 0 {
		params = []string{"charset=NONE"}
	}
	return Attach(env("FB_DATABASE", "employee"), params...)
}

// ServiceAddr is the host[:port] the Services managers take.
func ServiceAddr() string { return Host }

// ServiceOptions are the Services managers' default options.
func ServiceOptions() firebirdsql.ServiceManagerOptions {
	return firebirdsql.GetDefaultServiceManagerOptions()
}

// Scalar runs a single-value query.
func Scalar(q interface {
	QueryRow(string, ...any) *sql.Row
}, query string, args ...any) (any, error) {
	var v any
	err := q.QueryRow(query, args...).Scan(&v)
	return v, err
}

// Rows runs a query and returns every row as []any.
func Rows(q interface {
	Query(string, ...any) (*sql.Rows, error)
}, query string, args ...any) ([][]any, error) {
	rs, err := q.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rs.Close()
	cols, _ := rs.Columns()
	var out [][]any
	for rs.Next() {
		vals := make([]any, len(cols))
		ptrs := make([]any, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		if err := rs.Scan(ptrs...); err != nil {
			return nil, err
		}
		out = append(out, vals)
	}
	return out, rs.Err()
}

// Text renders a fetched value for display, NULL as <null>.
func Text(v any) string {
	switch x := v.(type) {
	case nil:
		return "<null>"
	case []byte:
		return string(x)
	default:
		return fmt.Sprint(x)
	}
}

// FbError unwraps the driver's structured error, if err is one.
func FbError(err error) (*firebirdsql.FbError, bool) {
	var fe *firebirdsql.FbError
	ok := errors.As(err, &fe)
	return fe, ok
}

// ErrText is an error's message, one status-vector entry per line.
func ErrText(err error) string {
	lines := []string{}
	for _, l := range strings.Split(err.Error(), "\n") {
		if strings.TrimSpace(l) != "" {
			lines = append(lines, l)
		}
	}
	return strings.Join(lines, "\n")
}

// Check exits with the engine's message when err is not nil.
func Check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", ErrText(err))
		os.Exit(1)
	}
}
