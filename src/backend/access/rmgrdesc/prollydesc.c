/*-------------------------------------------------------------------------
 *
 * prollydesc.c
 *	  rmgr descriptor routines for prolly storage (access/prolly)
 *
 * These are generic: they name the implementation that wrote a record and
 * its record type, but can't decode the payload, because pg_waldump has no
 * registry of implementations (open question 16).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/rmgrdesc/prollydesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_xlog.h"

static const char *const type_names[] = {
	"TYPE_0", "TYPE_1", "TYPE_2", "TYPE_3", "TYPE_4", "TYPE_5", "TYPE_6",
	"TYPE_7", "TYPE_8", "TYPE_9", "TYPE_10", "TYPE_11", "TYPE_12", "TYPE_13",
	"TYPE_14", "TYPE_15",
};

static void
generic_desc(StringInfo buf, XLogReaderState *record)
{
	xl_prolly_header hdr;

	if (XLogRecGetDataLen(record) < SizeOfProllyHeader)
	{
		appendStringInfoString(buf, "(no prolly header)");
		return;
	}
	memcpy(&hdr, XLogRecGetData(record), SizeOfProllyHeader);
	appendStringInfo(buf, "impl %u, %u bytes", hdr.impl_id,
					 (unsigned) (XLogRecGetDataLen(record) - SizeOfProllyHeader));
}

static const char *
generic_identify(uint8 info)
{
	return type_names[(info & PROLLY_XLOG_TYPE_MASK) >> 4];
}

void
prolly_desc(StringInfo buf, XLogReaderState *record)
{
	generic_desc(buf, record);
}

const char *
prolly_identify(uint8 info)
{
	return generic_identify(info);
}

void
prolly_store_desc(StringInfo buf, XLogReaderState *record)
{
	generic_desc(buf, record);
}

const char *
prolly_store_identify(uint8 info)
{
	return generic_identify(info);
}
