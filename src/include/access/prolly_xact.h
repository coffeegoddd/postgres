/*-------------------------------------------------------------------------
 *
 * prolly_xact.h
 *	  Prolly's session state: the branch a connection asks for, and who
 *	  its commits are by (defined with the dolt_* functions).
 *
 * src/include/access/prolly_xact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_XACT_H
#define PROLLY_XACT_H

/*
 * Per-session setup once connected to a database: defines the dolt.*
 * settings, and notes branch (from a "dbname/branch" connection) unless
 * NULL, to be checked out by prolly_session_ready(), called once the
 * backend is processing normally: a branch with only a head has its copies
 * made then, which takes DDL.
 */
extern void prolly_session_init(const char *branch);
extern void prolly_session_ready(void);

/* Who the session's commits are by: dolt.author, else the session user. */
extern const char *prolly_commit_author(void);
/* Commits name author until reset (NULL), whatever dolt.author says. */
extern void prolly_set_commit_author(const char *author);

/* Whether a transaction may commit with unresolved merge conflicts. */
extern bool prolly_allow_commit_conflicts(void);

#endif							/* PROLLY_XACT_H */
