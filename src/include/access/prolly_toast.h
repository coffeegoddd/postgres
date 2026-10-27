/*-------------------------------------------------------------------------
 *
 * prolly_toast.h
 *	  Versioned tables' values out of line, in their database's chunk store:
 *	  the TOAST destination of BLAKE3 hash pointers.
 *
 * A value is stored as a value tree (prolly_tree.h, value_write()) in the
 * database's chunk store, and named by a VARTAG_ONDISK_BLAKE3 pointer to
 * the tree's root.  The tree holds the value's canonical bytes
 * (prolly_datum.h) where they differ from the bytes it holds only in form,
 * so that the pointer's hash is the column's reference in history: a
 * text-like value's bytes in UTF-8 when the database's encoding is
 * another; a jsonb document's canonical bytes.  The form is the pointer's,
 * in va_extinfo's high bits; fetching undoes it, giving the value's bytes
 * back (a value whose conversion wouldn't is stored as its bytes).  Other
 * types' trees hold their bytes, which aren't their canonical bytes (a
 * pointer doesn't say its type, so fetching couldn't decode them): their
 * pointers' hashes aren't the columns' references.
 *
 * A long jsonb document is stored as a JSON tree (prolly_jsontree.h), and
 * a fold makes it the column's tree: a pointer of form
 * PROLLY_TOAST_FORM_JSON_TREE names its root, and a reference to it in a
 * row records length 0, which no value tree out of line has.  The
 * pointer's sizes are the document's jsonb's when it was stored, and an
 * estimate after an edit by path or read from history (never less than a
 * long document's): nothing canonical depends on them.
 *
 * src/include/access/prolly_toast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_TOAST_H
#define PROLLY_TOAST_H

#include "access/prolly_chunkstore.h"
#include "access/prolly_jsontree.h"
#include "access/prolly_tree.h"
#include "access/toast_destination.h"
#include "access/xlogdefs.h"
#include "lib/stringinfo.h"

/* What a hash pointer's tree holds */
#define PROLLY_TOAST_FORM_BYTES		0	/* the value's bytes */
#define PROLLY_TOAST_FORM_UTF8		1	/* a text-like value's, in UTF-8 */
#define PROLLY_TOAST_FORM_JSONB		2	/* a document's canonical bytes */
#define PROLLY_TOAST_FORM_JSON_TREE	3	/* a document's JSON tree */


/* the destination of the prolly access method's tables */
extern PGDLLIMPORT const ToastDestinationRoutine prolly_toast_chunkstore;

/*
 * Store a value of type typid (TOASTed or not) in the current database's
 * chunk store: the hash pointer naming it.
 */
extern varlena *prolly_toast_store(Datum value, Oid typid);

/*
 * The reference of the value tree a hash pointer names, appended to ref;
 * returns the pointer's form.
 */
extern int	prolly_toast_reference(const varlena *pointer, StringInfo ref);

/*
 * Whether this backend may read the value a hash pointer names now (its
 * tree's root is in the chunk store, and not only a transaction's in
 * progress).
 */
extern bool prolly_toast_readable(const varlena *pointer);

/*
 * Whether a hash pointer's tree is the column's value tree in history,
 * without reading the value: whether it holds the canonical bytes of a
 * value of type typid (prolly_datum.h), is the tree implementation's and
 * chunk store's that values (history's value store) writes, and is
 * there.  Then its reference is appended to ref, and the value's
 * canonical length is *len.
 */
extern bool prolly_toast_canonical(const varlena *pointer, Oid typid,
								   const ProllyValueStore *values,
								   StringInfo ref, uint64 *len);

/*
 * The hash pointer naming a value tree (root, len bytes) of a column of
 * type typid in history, if its canonical bytes are a form a pointer's tree
 * holds and the value's size is known without reading it; else NULL.
 */
extern varlena *prolly_toast_pointer(const ProllyHash *root, uint64 len,
									 Oid typid);

/*
 * Visit the root of every value tree a hash pointer on a page of the
 * current database's page stores names: every page, its items dead or
 * alive (shared buffers written first).  Conservative: bytes anywhere in an
 * item that spell a hash pointer are taken for one, so nothing a pointer
 * names is missed.  A collection's roots outside the refs.
 */
extern void prolly_toast_roots(ProllyChunkRefVisit visit, void *arg);
extern void prolly_toast_roots_in(uint32 lo, uint32 hi,
								  ProllyChunkRefVisit visit, void *arg);

/*
 * Visit the root of every value tree a hash pointer in the WAL from 'from'
 * to its end (flushed first) may name: a logical slot that decodes from
 * 'from' fetches the values its rows name, whose rows may be gone from the
 * pages since.  Conservative as the pages' scan is: bytes that spell a hash
 * pointer in a record's data, its blocks' data and its full-page images.
 * The WAL from 'from' must be here (ReplicationSlotsLogicalDecodingHorizonKept()
 * gives such a position); a segment removed while it is read is an error.
 */
extern void prolly_toast_wal_roots(XLogRecPtr from, ProllyChunkRefVisit visit,
								   void *arg);

/*
 * Whether a value is a hash pointer to a JSON tree: its root, and the
 * document's jsonb size as the pointer has it (*rawsize, with its header).
 */
extern bool prolly_toast_json_tree(const varlena *pointer, ProllyNodeRef *root,
								   Size *rawsize);

/* A hash pointer to the JSON tree at root, of a document of rawsize bytes. */
extern varlena *prolly_toast_json_pointer(const ProllyNodeRef *root,
										  Size rawsize);

/*
 * The environment values stored by content are written and read in, the
 * current database's: for the JSON trees hash pointers name.
 */
extern const ProllyTreeEnv *prolly_toast_env(void);

/*
 * The value tree nodes this backend read and wrote through this
 * destination: fetching and storing values by content (not a fold's,
 * apply's or collection's reads of trees, through history's environment).
 */
extern void prolly_toast_counters(ProllyNodeCounters *out, bool reset);

#endif							/* PROLLY_TOAST_H */
