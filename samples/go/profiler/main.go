// profiler - the accumulation view, driven from client code (Go twin of
// ../../cpp/profiler.cpp; see ../../../profiler.md).
//
// One RDB$PROFILER session brackets two workloads - a self-join over a
// 5,000-row table and a 20,000-iteration PSQL loop - then FINISH_SESSION(TRUE)
// flushes and PLG$PROFILER is queried like any other schema: the record-source
// view as an indented plan tree, the PSQL view ranked by total time.
//
// The control surface is a SQL package and the output a SQL schema, so a
// pure-Go wire client loses nothing.  What this twin adds is the
// autonomous-flush pitfall made visible instead of avoided: the session runs
// inside an explicit SNAPSHOT *sql.Tx (sql.LevelRepeatableRead), and a count
// of the flushed rows taken in that same transaction comes back 0 - the
// plugin committed them autonomously, after the snapshot was taken.  Once
// the Tx commits, the views are read through database/sql's implicit
// transaction, which in firebirdsql is READ COMMITTED with commit-retaining
// autocommit, so it sees the flush with no ceremony at all.
//
// Run:  go run ./profiler [database]
package main

import (
	"context"
	"database/sql"
	"fmt"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

const hotspot = `create procedure hotspot returns (total bigint) as
  declare i int = 0;
  declare x int;
begin
  total = 0;
  while (i < 20000) do
  begin
    select val from nums where id = mod(:i, 5000) into :x;
    total = total + coalesce(:x, 0);
    i = i + 1;
  end
  suspend;
end`

// table prints a query's result with isql-style column widths.
func table(db *sql.DB, query string, args ...any) {
	rs, err := db.Query(query, args...)
	fbsample.Check(err)
	defer rs.Close()
	cols, _ := rs.Columns()
	width := make([]int, len(cols))
	for i, c := range cols {
		width[i] = len(c)
	}
	var cells [][]string
	for rs.Next() {
		vals := make([]any, len(cols))
		ptrs := make([]any, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		fbsample.Check(rs.Scan(ptrs...))
		row := []string{}
		for i, v := range vals {
			s := strings.TrimRight(fbsample.Text(v), " ")
			row = append(row, s)
			width[i] = max(width[i], len(s))
		}
		cells = append(cells, row)
	}
	fbsample.Check(rs.Err())
	line := func(f []string) {
		for i, s := range f {
			f[i] = fmt.Sprintf("%-*s", width[i], s)
		}
		fmt.Println(strings.TrimRight(strings.Join(f, " "), " "))
	}
	line(append([]string{}, cols...))
	dashes := []string{}
	for _, w := range width {
		dashes = append(dashes, strings.Repeat("-", w))
	}
	line(dashes)
	for _, c := range cells {
		line(c)
	}
}

func main() {
	db, err := fbsample.Recreate(fbsample.DBPath("profiler"))
	fbsample.Check(err)
	defer db.Close()

	// --- workload fixtures: a table and a looping procedure ---------------
	for _, q := range []string{
		"create table nums (id int primary key, val int)",
		"execute block as declare n int = 0; begin " +
			"  while (n < 5000) do begin " +
			"    insert into nums values (:n, mod(:n, 97)); n = n + 1; end end",
		hotspot,
	} {
		_, err := db.Exec(q)
		fbsample.Check(err)
	}

	// --- profile inside an explicit SNAPSHOT transaction ------------------
	tx, err := db.BeginTx(context.Background(), &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
	fbsample.Check(err)
	var profileID int64
	fbsample.Check(tx.QueryRow(
		"select rdb$profiler.start_session('go hands-on') from rdb$database").Scan(&profileID))
	var n, total int64
	fbsample.Check(tx.QueryRow("select count(*) from nums a join nums b on b.id = a.val").Scan(&n))
	fbsample.Check(tx.QueryRow("select total from hotspot").Scan(&total))
	_, err = tx.Exec("execute procedure rdb$profiler.finish_session(true)")
	fbsample.Check(err)
	var seen int64
	fbsample.Check(tx.QueryRow("select count(*) from plg$profiler.plg$prof_psql_stats_view "+
		"where profile_id = ?", profileID).Scan(&seen))
	fmt.Printf("profile session %d finished and flushed\n", profileID)
	fmt.Printf("PSQL stat rows visible inside the SNAPSHOT that ran it: %d  "+
		"<- the flush committed autonomously, after the snapshot\n", seen)
	fbsample.Check(tx.Commit())

	// --- the plan tree, with per-operator counters and times --------------
	fmt.Println("\nrecord sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):")
	table(db, "select cast(lpad('', level * 2) || cast(access_path as varchar(120)) "+
		"           as varchar(140)) as access_path, "+
		"       open_counter as opens, fetch_counter as fetches, "+
		"       open_fetch_total_elapsed_time as total_ns "+
		"from plg$profiler.plg$prof_record_source_stats_view "+
		"where profile_id = ? and sql_text containing 'join nums' "+
		"order by cursor_id, record_source_id", profileID)

	// --- the PSQL hotspot, per line and column ----------------------------
	fmt.Println("\nhotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):")
	table(db, "select line_num, column_num, counter, "+
		"       total_elapsed_time as total_ns, avg_elapsed_time as avg_ns "+
		"from plg$profiler.plg$prof_psql_stats_view "+
		"where profile_id = ? and routine_name = 'HOTSPOT' "+
		"order by total_elapsed_time desc", profileID)
	fmt.Println("\ndone.")
}
