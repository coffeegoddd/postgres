/*-------------------------------------------------------------------------
 *
 * ws_tags.c
 *	  Column references by tag in stored text (prolly/ws/ws_tags.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_tags.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/ws/ws_tags.h"
#include "utils/builtins.h"

void
ws_tags_escape(const char *text, Buf *out)
{
	for (const char *p = text; *p; p++)
	{
		if (*p == '@')
			buf_append_byte(out, '@');
		buf_append_byte(out, (uint8) *p);
	}
}

pg_noreturn static void
malformed(const char *stored)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("malformed column references in stored text: \"%s\"", stored)));
}

bool
ws_tags_render(const char *stored, WsTagNameFn name, void *arg, Buf *out, uint64 *missing)
{
	for (const char *p = stored; *p;)
	{
		uint64		tag = 0;
		const char *q;
		const char *n;
		const char *quoted;

		if (*p != '@')
		{
			buf_append_byte(out, (uint8) *p++);
			continue;
		}
		if (p[1] == '@')
		{
			buf_append_byte(out, '@');
			p += 2;
			continue;
		}
		if (p[1] != '{' || !isdigit((unsigned char) p[2]))
			malformed(stored);
		for (q = p + 2; isdigit((unsigned char) *q); q++)
		{
			uint64		d = (uint64) (*q - '0');

			if (tag > (PG_UINT64_MAX - d) / 10)
				malformed(stored);
			tag = tag * 10 + d;
		}
		if (*q != '}')
			malformed(stored);
		n = name(arg, tag);
		if (n == NULL)
		{
			*missing = tag;
			return false;
		}
		quoted = quote_identifier(n);
		buf_append(out, quoted, strlen(quoted));
		if (quoted != n)
			pfree((char *) quoted);
		p = q + 1;
	}
	return true;
}
