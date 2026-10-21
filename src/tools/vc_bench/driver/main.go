// A persistent SQL connection, controlled by JSON lines. Timing ends only after
// every row and result set has been drained. Neither JSON IPC nor data generation
// is part of query latency. Errors are returned, never converted into timings.
package main

import (
	"bufio"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
	_ "github.com/jackc/pgx/v5/stdlib"
)

type request struct {
	SQL     string `json:"sql"`
	Start   int64  `json:"start"`
	Count   int    `json:"count"`
	Width   int    `json:"width"`
	Timeout int    `json:"timeout"`
}

type response struct {
	Rows    [][]string `json:"rows"`
	Elapsed int64      `json:"elapsed_ns"`
	Error   string     `json:"error,omitempty"`
}

// The payload is identical across engines and batches, with no repeating filler.
// Base64 is SQL-literal safe; its entropy is about 6 bits per stored byte.
func payload(id int64, width int) string {
	b := make([]byte, 0, (width*3/4)+32)
	var input [16]byte
	binary.LittleEndian.PutUint64(input[:8], uint64(id))
	for block := uint64(0); len(b)*4/3 < width+4; block++ {
		binary.LittleEndian.PutUint64(input[8:], block)
		h := sha256.Sum256(input[:])
		b = append(b, h[:]...)
	}
	return base64.StdEncoding.EncodeToString(b)[:width]
}

func execute(c *sql.Conn, r request) (out response) {
	if r.Timeout <= 0 {
		r.Timeout = 3600
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(r.Timeout)*time.Second)
	defer cancel()
	q := r.SQL
	if r.Count > 0 {
		if r.Count > 10000 || r.Width < 1 || r.Width > 16384 || r.Start < 1 {
			out.Error = "invalid load batch"
			return
		}
		var b strings.Builder
		b.WriteString("INSERT INTO bench_rows (id, revision, payload) VALUES ")
		for i := 0; i < r.Count; i++ {
			if i > 0 {
				b.WriteByte(',')
			}
			id := r.Start + int64(i)
			fmt.Fprintf(&b, "(%d,0,'%s')", id, payload(id, r.Width))
		}
		q = b.String()
	}
	start := time.Now()
	rows, err := c.QueryContext(ctx, q)
	if err == nil {
		for {
			cols, e := rows.Columns()
			if e != nil {
				err = e
				break
			}
			for rows.Next() {
				values := make([]sql.NullString, len(cols))
				args := make([]any, len(cols))
				for i := range values {
					args[i] = &values[i]
				}
				if e := rows.Scan(args...); e != nil {
					err = e
					break
				}
				row := make([]string, len(cols))
				for i, v := range values {
					if v.Valid {
						row[i] = v.String
					} else {
						row[i] = "<NULL>"
					}
				}
				// Our queries return scalar aggregates, status or a few probes only.
				if len(out.Rows) < 100 {
					out.Rows = append(out.Rows, row)
				}
			}
			if err != nil || !rows.NextResultSet() {
				break
			}
		}
		if err == nil {
			err = rows.Err()
		}
		if e := rows.Close(); err == nil {
			err = e
		}
	}
	out.Elapsed = time.Since(start).Nanoseconds()
	if err != nil {
		out.Error = err.Error()
	}
	return
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: sql-driver DRIVER DSN")
		os.Exit(2)
	}
	db, err := sql.Open(os.Args[1], os.Args[2])
	if err != nil {
		panic(err)
	}
	defer db.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	c, err := db.Conn(ctx)
	cancel()
	if err != nil {
		panic(err)
	}
	defer c.Close()
	sc := bufio.NewScanner(os.Stdin)
	sc.Buffer(make([]byte, 65536), 32<<20)
	enc := json.NewEncoder(os.Stdout)
	for sc.Scan() {
		var r request
		if err := json.Unmarshal(sc.Bytes(), &r); err != nil {
			_ = enc.Encode(response{Error: err.Error()})
			continue
		}
		if err := enc.Encode(execute(c, r)); err != nil {
			panic(err)
		}
	}
	if err := sc.Err(); err != nil {
		panic(err)
	}
}
