/*-------------------------------------------------------------------------
 *
 * trace.h
 *	  Elapsed-time tracing of the engine's steps, for finding where an
 *	  operation's time goes: with messages at DEBUG2 logged (or sent to the
 *	  client), each traced step is a message,
 *
 *		trace <depth-indented name> <seconds> [detail]
 *
 *	nested steps indented under the step that contains them.  Off (by
 *	default), a traced step costs one test of the message level.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/engine/trace.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ENGINE_TRACE_H
#define PROLLY_ENGINE_TRACE_H

#include "portability/instr_time.h"
#include "utils/elog.h"

extern PGDLLIMPORT int trace_depth;

typedef struct TraceSpan
{
	const char *name;
	instr_time	t0;
	bool		on;
} TraceSpan;

static inline TraceSpan
trace_begin(const char *name)
{
	TraceSpan	s = {.name = name,.on = message_level_is_interesting(DEBUG2)};

	if (s.on)
	{
		INSTR_TIME_SET_CURRENT(s.t0);
		trace_depth++;
	}
	return s;
}

/* Ends a step: its time, and a detail (printf-style) if given. */
#define trace_end(span, ...) \
	do { \
		TraceSpan *s_ = &(span); \
		if (s_->on) \
		{ \
			instr_time	now_; \
			char		detail_[256]; \
			INSTR_TIME_SET_CURRENT(now_); \
			INSTR_TIME_SUBTRACT(now_, s_->t0); \
			trace_depth--; \
			snprintf(detail_, sizeof(detail_), " " __VA_ARGS__); \
			elog(DEBUG2, "trace %*s%-*s %9.4f s%s", trace_depth * 2, "", \
				 32 - trace_depth * 2, s_->name, INSTR_TIME_GET_DOUBLE(now_), \
				 detail_); \
		} \
	} while (0)

#endif							/* PROLLY_ENGINE_TRACE_H */
