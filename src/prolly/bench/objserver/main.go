// objserver: a small object store over HTTP with the verbs a plain-storage
// remote uses (S3's shape, not its signing): PUT, conditional PUT, GET with
// a byte range, HEAD, DELETE, and LIST.  For the prototype's remote
// measurements: no downloads, an injectable delay per request, and counters
// of requests and bytes.
//
//	PUT    /<key>              store; If-Match: <etag> or If-None-Match: *
//	                           make it conditional (412 if not met)
//	GET    /<key>              the object; Range: bytes=a-b gives 206
//	HEAD   /<key>              its size and ETag
//	DELETE /<key>
//	GET    /?list=<prefix>     keys under the prefix, one "key size" a line
//	GET    /_stats             requests and bytes since the last reset
//	POST   /_stats/reset
//
// Objects are files under -dir (written to a temporary name, then renamed);
// an object's ETag is the SHA-256 of its bytes.  Writes to one key are
// serialized, so a conditional PUT is a compare-and-swap.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var (
	dir   = flag.String("dir", "objects", "where objects are kept")
	addr  = flag.String("addr", "127.0.0.1:9100", "listen address")
	delay = flag.Duration("delay", 0, "added to every request (simulated latency)")

	keyLocks sync.Map // key -> *sync.Mutex

	nreq, nget, nput, nhead, nlist, nbytesIn, nbytesOut atomic.Int64
)

func lockKey(key string) *sync.Mutex {
	m, _ := keyLocks.LoadOrStore(key, &sync.Mutex{})
	return m.(*sync.Mutex)
}

func pathOf(key string) (string, bool) {
	clean := filepath.Clean("/" + key)
	if clean == "/" || strings.Contains(clean, "..") {
		return "", false
	}
	return filepath.Join(*dir, clean), true
}

func etagOf(path string) (string, int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return "", 0, err
	}
	return "\"" + hex.EncodeToString(h.Sum(nil)) + "\"", n, nil
}

func handle(w http.ResponseWriter, r *http.Request) {
	if *delay > 0 {
		time.Sleep(*delay)
	}
	if r.URL.Path == "/_stats" || r.URL.Path == "/_stats/reset" {
		stats(w, r)
		return
	}
	nreq.Add(1)
	if r.URL.Path == "/" && r.Method == http.MethodGet {
		nlist.Add(1)
		list(w, r.URL.Query().Get("list"))
		return
	}
	key := strings.TrimPrefix(r.URL.Path, "/")
	path, ok := pathOf(key)
	if !ok {
		http.Error(w, "bad key", http.StatusBadRequest)
		return
	}
	switch r.Method {
	case http.MethodPut:
		nput.Add(1)
		put(w, r, key, path)
	case http.MethodGet:
		nget.Add(1)
		get(w, r, path)
	case http.MethodHead:
		nhead.Add(1)
		etag, n, err := etagOf(path)
		if err != nil {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		w.Header().Set("ETag", etag)
		w.Header().Set("Content-Length", strconv.FormatInt(n, 10))
	case http.MethodDelete:
		m := lockKey(key)
		m.Lock()
		os.Remove(path)
		m.Unlock()
		w.WriteHeader(http.StatusNoContent)
	default:
		http.Error(w, "method", http.StatusMethodNotAllowed)
	}
}

func put(w http.ResponseWriter, r *http.Request, key, path string) {
	m := lockKey(key)
	m.Lock()
	defer m.Unlock()
	cur, _, err := etagOf(path)
	exists := err == nil
	if want := r.Header.Get("If-Match"); want != "" && (!exists || want != cur) {
		http.Error(w, "precondition failed", http.StatusPreconditionFailed)
		return
	}
	if r.Header.Get("If-None-Match") == "*" && exists {
		http.Error(w, "precondition failed", http.StatusPreconditionFailed)
		return
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	tmp := path + ".tmp"
	f, err := os.Create(tmp)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), r.Body)
	if err == nil {
		err = f.Sync()
	}
	f.Close()
	if err != nil {
		os.Remove(tmp)
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if err := os.Rename(tmp, path); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	nbytesIn.Add(n)
	w.Header().Set("ETag", "\""+hex.EncodeToString(h.Sum(nil))+"\"")
	w.WriteHeader(http.StatusOK)
}

func get(w http.ResponseWriter, r *http.Request, path string) {
	f, err := os.Open(path)
	if err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	size := st.Size()
	if rg := r.Header.Get("Range"); strings.HasPrefix(rg, "bytes=") {
		parts := strings.SplitN(strings.TrimPrefix(rg, "bytes="), "-", 2)
		lo, err1 := strconv.ParseInt(parts[0], 10, 64)
		hi, err2 := strconv.ParseInt(parts[1], 10, 64)
		if err1 != nil || err2 != nil || lo < 0 || hi < lo || lo >= size {
			http.Error(w, "range", http.StatusRequestedRangeNotSatisfiable)
			return
		}
		if hi >= size {
			hi = size - 1
		}
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", lo, hi, size))
		w.Header().Set("Content-Length", strconv.FormatInt(hi-lo+1, 10))
		w.WriteHeader(http.StatusPartialContent)
		n, _ := io.Copy(w, io.NewSectionReader(f, lo, hi-lo+1))
		nbytesOut.Add(n)
		return
	}
	w.Header().Set("Content-Length", strconv.FormatInt(size, 10))
	n, _ := io.Copy(w, f)
	nbytesOut.Add(n)
}

func list(w http.ResponseWriter, prefix string) {
	var lines []string
	filepath.Walk(*dir, func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() || strings.HasSuffix(p, ".tmp") {
			return nil
		}
		rel, _ := filepath.Rel(*dir, p)
		if strings.HasPrefix(rel, prefix) {
			lines = append(lines, fmt.Sprintf("%s %d", rel, info.Size()))
		}
		return nil
	})
	sort.Strings(lines)
	for _, l := range lines {
		fmt.Fprintln(w, l)
	}
}

func stats(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/_stats/reset" {
		for _, c := range []*atomic.Int64{&nreq, &nget, &nput, &nhead, &nlist, &nbytesIn, &nbytesOut} {
			c.Store(0)
		}
		return
	}
	fmt.Fprintf(w, "requests %d\ngets %d\nputs %d\nheads %d\nlists %d\nbytes_in %d\nbytes_out %d\n",
		nreq.Load(), nget.Load(), nput.Load(), nhead.Load(), nlist.Load(), nbytesIn.Load(), nbytesOut.Load())
}

func main() {
	flag.Parse()
	if err := os.MkdirAll(*dir, 0o755); err != nil {
		log.Fatal(err)
	}
	log.Printf("objserver on %s, objects in %s, delay %s", *addr, *dir, *delay)
	log.Fatal(http.ListenAndServe(*addr, http.HandlerFunc(handle)))
}
