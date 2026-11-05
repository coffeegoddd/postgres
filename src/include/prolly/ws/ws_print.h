/*-------------------------------------------------------------------------
 *
 * ws_print.h
 *	  What a restore printed of what a commit holds.
 *
 * What reads back from a commit's text may print otherwise, with nothing changed, as
 * Postgres's printing is not a fixed point.  So a restore remembers, of what printed
 * otherwise, what it printed as, and a read holds the commit's text while it still
 * does.  Any change makes it print otherwise, and a read then holds that print.
 *
 * An entry is only worth anything while what it remembers is printed: one out of date
 * is never used, and one lost only makes what it was for read as changed.
 *
 * Text here is as a commit holds it, in UTF-8.
 *
 * src/include/prolly/ws/ws_print.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WS_PRINT_H
#define WS_PRINT_H

typedef struct WsPrintEntry
{
	char	   *key;			/* the object, as a commit keys it */
	char	   *printed;		/* what it printed once restored */
	char	   *held;			/* what the commit held of it */
} WsPrintEntry;

typedef struct WsPrintStore
{
	/* The entries of a schema, palloc'd, of *n: at what WsSql reads now. */
	WsPrintEntry *(*load) (const char *schema, int *n);
	void		(*remember) (const char *schema, const char *key, const char *printed, const char *held);
	void		(*forget) (const char *schema, const char *key);
} WsPrintStore;

extern const WsPrintStore *const WsPrints;

/* As a schema is dropped. */
extern void WsPrintsForgetSchema(Oid nspoid);

#endif							/* WS_PRINT_H */
