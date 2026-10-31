/*
 * remote.c
 *	  Plain-storage remotes: a directory, or an object server over HTTP.
 *
 * The directory backend reads and writes files (a conditional put under an
 * exclusive lock on a sibling lock file, the ETag a SHA-256 of the bytes).
 * The HTTP backend speaks PUT, ranged GET, HEAD and LIST to bench/objserver
 * (S3's verbs) with libcurl: one kept-alive connection for single calls, a
 * multi handle with up to 'parallel' connections for remote_get_many().
 */
#include "remote.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef enum
{
	REMOTE_DIR,
	REMOTE_HTTP
} RemoteKind;

struct Remote
{
	RemoteKind	kind;
	char	   *base;
	int			parallel;
	CURL	   *easy;
	CURLM	   *multi;
	CURL	  **handles;
	RemoteStats stats;
	char		desc[512];
};

static double
now_secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
sha256_etag(const uint8_t *data, size_t len, char *out)
{
	unsigned char md[32];
	unsigned int mdlen;

	EVP_Digest(data, len, md, &mdlen, EVP_sha256(), NULL);
	out[0] = '"';
	for (int i = 0; i < 32; i++)
		sprintf(out + 1 + i * 2, "%02x", md[i]);
	out[65] = '"';
	out[66] = '\0';
}

static void *
xmalloc(size_t n)
{
	void	   *p = malloc(n ? n : 1);

	if (p == NULL)
	{
		fprintf(stderr, "remote: out of memory\n");
		abort();
	}
	return p;
}

/* ------------------------------------------------------------ directory */

static char *
dir_path(Remote *r, const char *key)
{
	size_t		n = strlen(r->base) + strlen(key) + 2;
	char	   *p = xmalloc(n);

	snprintf(p, n, "%s/%s", r->base, key);
	return p;
}

static int
mkdirs_for(const char *path)
{
	char	   *p = strdup(path);

	for (char *s = p + 1; *s; s++)
		if (*s == '/')
		{
			*s = '\0';
			if (mkdir(p, 0755) != 0 && errno != EEXIST)
			{
				free(p);
				return -1;
			}
			*s = '/';
		}
	free(p);
	return 0;
}

static int
dir_write(const char *path, const uint8_t *data, size_t len)
{
	size_t		n = strlen(path) + 5;
	char	   *tmp = xmalloc(n);
	int			fd;
	size_t		off = 0;

	snprintf(tmp, n, "%s.tmp", path);
	if (mkdirs_for(path) != 0 ||
		(fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
	{
		free(tmp);
		return -1;
	}
	while (off < len)
	{
		ssize_t		w = write(fd, data + off, len - off);

		if (w <= 0)
		{
			close(fd);
			free(tmp);
			return -1;
		}
		off += (size_t) w;
	}
	if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0)
	{
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

static uint8_t *
dir_read(const char *path, uint64_t off, uint64_t len, size_t *got)
{
	int			fd = open(path, O_RDONLY);
	struct stat st;
	uint8_t    *buf;
	size_t		done = 0;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) != 0 || off > (uint64_t) st.st_size)
	{
		close(fd);
		return NULL;
	}
	if (len == 0 || off + len > (uint64_t) st.st_size)
		len = (uint64_t) st.st_size - off;
	buf = xmalloc(len);
	while (done < len)
	{
		ssize_t		r = pread(fd, buf + done, len - done, (off_t) (off + done));

		if (r <= 0)
		{
			close(fd);
			free(buf);
			return NULL;
		}
		done += (size_t) r;
	}
	close(fd);
	*got = done;
	return buf;
}

/* ----------------------------------------------------------------- http */

typedef struct Buf
{
	uint8_t    *data;
	size_t		len;
	size_t		cap;
} Buf;

static size_t
buf_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
	Buf		   *b = ud;
	size_t		n = size * nmemb;

	if (b->len + n > b->cap)
	{
		b->cap = (b->len + n) * 2;
		b->data = realloc(b->data, b->cap);
		if (b->data == NULL)
			abort();
	}
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	return n;
}

typedef struct Hdr
{
	char		etag[REMOTE_ETAG_LEN];
} Hdr;

static size_t
hdr_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
	Hdr		   *h = ud;
	size_t		n = size * nmemb;

	if (n > 5 && strncasecmp(ptr, "etag:", 5) == 0)
	{
		const char *v = ptr + 5;
		size_t		k;

		while (*v == ' ')
			v++;
		k = strcspn(v, "\r\n");
		if (k >= REMOTE_ETAG_LEN)
			k = REMOTE_ETAG_LEN - 1;
		memcpy(h->etag, v, k);
		h->etag[k] = '\0';
	}
	return n;
}

static char *
http_url(Remote *r, const char *key)
{
	size_t		n = strlen(r->base) + strlen(key) + 2;
	char	   *u = xmalloc(n);

	snprintf(u, n, "%s/%s", r->base, key);
	return u;
}

static void
http_prepare_get(Remote *r, CURL *c, const char *key, uint64_t off,
				 uint64_t len, Buf *b, Hdr *h)
{
	char	   *u = http_url(r, key);
	char		range[64];

	curl_easy_reset(c);
	curl_easy_setopt(c, CURLOPT_URL, u);
	free(u);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_write);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, h);
	curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
	if (off > 0 || len > 0)
	{
		if (len > 0)
			snprintf(range, sizeof(range), "%llu-%llu",
					 (unsigned long long) off, (unsigned long long) (off + len - 1));
		else
			snprintf(range, sizeof(range), "%llu-", (unsigned long long) off);
		curl_easy_setopt(c, CURLOPT_RANGE, range);
	}
}

/* --------------------------------------------------------------- common */

Remote *
remote_dir_open(const char *dir)
{
	Remote	   *r = calloc(1, sizeof(Remote));

	r->kind = REMOTE_DIR;
	r->base = strdup(dir);
	r->parallel = 1;
	mkdir(dir, 0755);
	snprintf(r->desc, sizeof(r->desc), "file://%s", dir);
	return r;
}

Remote *
remote_http_open(const char *base_url, int parallel)
{
	Remote	   *r = calloc(1, sizeof(Remote));
	static bool inited = false;

	if (!inited)
	{
		curl_global_init(CURL_GLOBAL_DEFAULT);
		inited = true;
	}
	r->kind = REMOTE_HTTP;
	r->base = strdup(base_url);
	r->parallel = parallel > 0 ? parallel : 1;
	r->easy = curl_easy_init();
	r->multi = curl_multi_init();
	curl_multi_setopt(r->multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long) r->parallel);
	r->handles = calloc((size_t) r->parallel, sizeof(CURL *));
	for (int i = 0; i < r->parallel; i++)
		r->handles[i] = curl_easy_init();
	snprintf(r->desc, sizeof(r->desc), "%s (%d parallel)", base_url, r->parallel);
	return r;
}

void
remote_close(Remote *r)
{
	if (r->kind == REMOTE_HTTP)
	{
		for (int i = 0; i < r->parallel; i++)
			curl_easy_cleanup(r->handles[i]);
		free(r->handles);
		curl_multi_cleanup(r->multi);
		curl_easy_cleanup(r->easy);
	}
	free(r->base);
	free(r);
}

RemoteStats *
remote_stats(Remote *r)
{
	return &r->stats;
}

const char *
remote_describe(Remote *r)
{
	return r->desc;
}

int
remote_put(Remote *r, const char *key, const uint8_t *data, size_t len)
{
	char		etag[REMOTE_ETAG_LEN];

	return remote_put_if(r, key, data, len, "", etag) == 0 ? 0 : -1;
}

/* (if_match "": unconditional) */
int
remote_put_if(Remote *r, const char *key, const uint8_t *data, size_t len,
			  const char *if_match, char *etag_out)
{
	double		t0 = now_secs();
	int			rc = -1;

	r->stats.requests++;
	r->stats.puts++;
	r->stats.bytes_up += len;
	if (r->kind == REMOTE_DIR)
	{
		char	   *path = dir_path(r, key);
		size_t		ln = strlen(path) + 6;
		char	   *lock = xmalloc(ln);
		int			lfd;

		snprintf(lock, ln, "%s.lock", path);
		mkdirs_for(path);
		lfd = open(lock, O_RDWR | O_CREAT, 0644);
		if (lfd >= 0 && flock(lfd, LOCK_EX) == 0)
		{
			size_t		cur_len;
			uint8_t    *cur = dir_read(path, 0, 0, &cur_len);
			char		cur_etag[REMOTE_ETAG_LEN];
			bool		ok = true;

			if (cur)
				sha256_etag(cur, cur_len, cur_etag);
			if (if_match == NULL)
				ok = cur == NULL;
			else if (if_match[0] != '\0')
				ok = cur != NULL && strcmp(cur_etag, if_match) == 0;
			free(cur);
			if (!ok)
				rc = 1;
			else if (dir_write(path, data, len) == 0)
			{
				sha256_etag(data, len, etag_out);
				rc = 0;
			}
			flock(lfd, LOCK_UN);
		}
		if (lfd >= 0)
			close(lfd);
		free(lock);
		free(path);
	}
	else
	{
		CURL	   *c = r->easy;
		char	   *u = http_url(r, key);
		struct curl_slist *hl = NULL;
		char		cond[REMOTE_ETAG_LEN + 16];
		Hdr			h = {{0}};
		Buf			b = {0};
		long		code = 0;

		curl_easy_reset(c);
		curl_easy_setopt(c, CURLOPT_URL, u);
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, (const char *) data);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) len);
		curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
		hl = curl_slist_append(hl, "Expect:");
		hl = curl_slist_append(hl, "Content-Type: application/octet-stream");
		if (if_match == NULL)
			hl = curl_slist_append(hl, "If-None-Match: *");
		else if (if_match[0] != '\0')
		{
			snprintf(cond, sizeof(cond), "If-Match: %s", if_match);
			hl = curl_slist_append(hl, cond);
		}
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_write);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, &h);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
		if (curl_easy_perform(c) == CURLE_OK)
		{
			curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
			if (code == 200)
			{
				strcpy(etag_out, h.etag);
				rc = 0;
			}
			else if (code == 412)
				rc = 1;
		}
		curl_slist_free_all(hl);
		free(b.data);
		free(u);
	}
	r->stats.secs += now_secs() - t0;
	return rc;
}

uint8_t *
remote_get(Remote *r, const char *key, uint64_t off, uint64_t len,
		   size_t *got, char *etag_out)
{
	double		t0 = now_secs();
	uint8_t    *out = NULL;

	r->stats.requests++;
	r->stats.gets++;
	if (r->kind == REMOTE_DIR)
	{
		char	   *path = dir_path(r, key);

		out = dir_read(path, off, len, got);
		if (out && etag_out && off == 0 && len == 0)
			sha256_etag(out, *got, etag_out);
		free(path);
	}
	else
	{
		Buf			b = {0};
		Hdr			h = {{0}};
		long		code = 0;

		http_prepare_get(r, r->easy, key, off, len, &b, &h);
		if (curl_easy_perform(r->easy) == CURLE_OK)
		{
			curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &code);
			if (code == 200 || code == 206)
			{
				out = b.data ? b.data : xmalloc(1);
				*got = b.len;
				b.data = NULL;
				if (etag_out)
					strcpy(etag_out, h.etag);
			}
		}
		free(b.data);
	}
	if (out)
		r->stats.bytes_down += *got;
	r->stats.secs += now_secs() - t0;
	return out;
}

bool
remote_get_many(Remote *r, RemoteRange *ranges, size_t n)
{
	double		t0 = now_secs();
	bool		ok = true;

	if (r->kind == REMOTE_DIR)
	{
		for (size_t i = 0; i < n; i++)
		{
			char	   *path = dir_path(r, ranges[i].key);

			ranges[i].data = dir_read(path, ranges[i].off, ranges[i].len, &ranges[i].got);
			free(path);
			r->stats.requests++;
			r->stats.gets++;
			if (ranges[i].data)
				r->stats.bytes_down += ranges[i].got;
		}
	}
	else
	{
		Buf		   *bufs = calloc(n, sizeof(Buf));
		Hdr		   *hdrs = calloc(n, sizeof(Hdr));
		size_t	   *slot_of = calloc((size_t) r->parallel, sizeof(size_t));
		bool	   *busy = calloc((size_t) r->parallel, sizeof(bool));
		size_t		next = 0,
					done = 0;

		while (done < n)
		{
			int			running,
						left;
			CURLMsg    *m;

			for (int s = 0; s < r->parallel && next < n; s++)
				if (!busy[s])
				{
					http_prepare_get(r, r->handles[s], ranges[next].key,
									 ranges[next].off, ranges[next].len,
									 &bufs[next], &hdrs[next]);
					curl_easy_setopt(r->handles[s], CURLOPT_PRIVATE, (void *) (intptr_t) s);
					curl_multi_add_handle(r->multi, r->handles[s]);
					slot_of[s] = next++;
					busy[s] = true;
					r->stats.requests++;
					r->stats.gets++;
				}
			curl_multi_perform(r->multi, &running);
			while ((m = curl_multi_info_read(r->multi, &left)) != NULL)
			{
				intptr_t	s;
				long		code = 0;
				size_t		i;

				if (m->msg != CURLMSG_DONE)
					continue;
				curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **) &s);
				i = slot_of[s];
				curl_easy_getinfo(m->easy_handle, CURLINFO_RESPONSE_CODE, &code);
				if (m->data.result == CURLE_OK && (code == 200 || code == 206))
				{
					ranges[i].data = bufs[i].data ? bufs[i].data : xmalloc(1);
					ranges[i].got = bufs[i].len;
					r->stats.bytes_down += bufs[i].len;
				}
				else
				{
					free(bufs[i].data);
					ranges[i].data = NULL;
					ok = false;
				}
				curl_multi_remove_handle(r->multi, m->easy_handle);
				busy[s] = false;
				done++;
			}
			if (done < n)
				curl_multi_poll(r->multi, NULL, 0, 100, NULL);
		}
		free(bufs);
		free(hdrs);
		free(slot_of);
		free(busy);
	}
	r->stats.secs += now_secs() - t0;
	return ok;
}

char **
remote_list(Remote *r, const char *prefix, size_t *n)
{
	char	  **keys = NULL;
	size_t		cap = 0;
	uint8_t    *body = NULL;
	size_t		len = 0;
	double		t0 = now_secs();

	*n = 0;
	r->stats.requests++;
	r->stats.lists++;
	if (r->kind == REMOTE_DIR)
	{
		size_t		cn = strlen(r->base) + strlen(prefix) + 64;
		char	   *cmd = xmalloc(cn);
		FILE	   *p;
		char		line[4096];

		/* (a prefix is a directory of the remote here: packs/) */
		snprintf(cmd, cn, "cd '%s' 2>/dev/null && ls -1 %s 2>/dev/null", r->base, prefix);
		p = popen(cmd, "r");
		free(cmd);
		while (p && fgets(line, sizeof(line), p))
		{
			line[strcspn(line, "\n")] = '\0';
			if (strstr(line, ".tmp") || strstr(line, ".lock"))
				continue;
			if (*n == cap)
				keys = realloc(keys, (cap = cap ? cap * 2 : 64) * sizeof(char *));
			keys[(*n)++] = strdup(line);
		}
		if (p)
			pclose(p);
	}
	else
	{
		size_t		un = strlen(prefix) + 16;
		char	   *q = xmalloc(un);
		char	   *s,
				   *save;

		snprintf(q, un, "?list=%s", prefix);
		{
			Buf			b = {0};
			Hdr			h = {{0}};
			char	   *u = http_url(r, "");

			curl_easy_reset(r->easy);
			{
				size_t		fn = strlen(u) + strlen(q) + 1;
				char	   *full = xmalloc(fn);

				snprintf(full, fn, "%s%s", u, q);
				curl_easy_setopt(r->easy, CURLOPT_URL, full);
				curl_easy_setopt(r->easy, CURLOPT_WRITEFUNCTION, buf_write);
				curl_easy_setopt(r->easy, CURLOPT_WRITEDATA, &b);
				curl_easy_setopt(r->easy, CURLOPT_HEADERFUNCTION, hdr_write);
				curl_easy_setopt(r->easy, CURLOPT_HEADERDATA, &h);
				curl_easy_perform(r->easy);
				free(full);
			}
			free(u);
			body = b.data;
			len = b.len;
		}
		free(q);
		if (body)
		{
			body = realloc(body, len + 1);
			body[len] = '\0';
			r->stats.bytes_down += len;
			for (s = strtok_r((char *) body, "\n", &save); s; s = strtok_r(NULL, "\n", &save))
			{
				char	   *sp = strchr(s, ' ');

				if (sp)
					*sp = '\0';
				if (*n == cap)
					keys = realloc(keys, (cap = cap ? cap * 2 : 64) * sizeof(char *));
				keys[(*n)++] = strdup(s);
			}
			free(body);
		}
	}
	r->stats.secs += now_secs() - t0;
	return keys;
}
