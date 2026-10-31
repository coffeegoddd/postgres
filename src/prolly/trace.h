/*
 * trace.h
 *	  Elapsed-time tracing of the engine's steps, for finding where an
 *	  operation's time goes: with PROLLY_TRACE set in the environment, each
 *	  traced step prints a line to stderr,
 *
 *		trace <depth-indented name> <seconds> [detail]
 *
 *	nested steps indented under the step that contains them.  Off, a traced
 *	step costs one branch.
 */
#ifndef PROLLY_TRACE_H
#define PROLLY_TRACE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline bool
trace_on(void)
{
	static int	on = -1;

	if (on < 0)
		on = getenv("PROLLY_TRACE") != NULL;
	return on;
}

static inline double
trace_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

extern int	trace_depth;

typedef struct TraceSpan
{
	const char *name;
	double		t0;
	bool		on;
} TraceSpan;

static inline TraceSpan
trace_begin(const char *name)
{
	TraceSpan	s = {name, 0, trace_on()};

	if (s.on)
	{
		s.t0 = trace_now();
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
			trace_depth--; \
			fprintf(stderr, "trace %*s%-*s %9.4f s", trace_depth * 2, "", \
					32 - trace_depth * 2, s_->name, trace_now() - s_->t0); \
			fprintf(stderr, " " __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

#endif							/* PROLLY_TRACE_H */
