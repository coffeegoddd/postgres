/*-------------------------------------------------------------------------
 *
 * prolly_tablefile.h
 *	  Table file format interface: an immutable file of chunks, with an
 *	  index of their hashes.
 *
 * A chunk store keeps chunks it doesn't append one at a time (those
 * received from elsewhere, or rewritten together) in table files.  This
 * interface is the files' format: how chunks are laid out, compressed and
 * found.  Where the files are, and how they are named, made durable and
 * listed, is the chunk store's.
 *
 * Contract:
 * - write_begin() starts a file; write_add() adds a chunk, appending the
 *   bytes that hold it to out (the caller writes them at the file's end, in
 *   order, and may empty out between calls); write_finish() appends the
 *   rest of the file.  A chunk is added at most once per file.  The same
 *   chunks added in the same order give the same bytes.
 * - open() reads a file's index, from its end, through read (which reads
 *   len bytes at offset into buf, or fails); NULL if the bytes aren't a
 *   whole file of the format.  If map is given, the format may use the
 *   bytes it gives (len bytes at offset, valid until the caller is done with
 *   the file, after close()) instead of reading them into memory; map
 *   returns NULL when it can't.  find() says where a chunk's bytes are in the
 *   file (false: not in it); decode() turns those bytes back into the chunk,
 *   appended to out, failing on corruption.
 * - count() and entry() list the file's chunks, in hash order; locate()
 *   gives a chunk's place in that order (-1: not in the file).
 * - version(), optional, says which version of the format a file open()
 *   refused is written in, if it is one of the format's at all (0: it
 *   isn't), so a file of another version is told from a damaged one.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_tablefile.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_TABLEFILE_H
#define PROLLY_TABLEFILE_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"

typedef struct ProllyTableFileWriter ProllyTableFileWriter;
typedef struct ProllyTableFile ProllyTableFile;

/* Read len bytes at offset into buf; false if the file has no such bytes. */
typedef bool (*ProllyTableFileRead) (void *arg, uint64 offset, char *buf,
									 Size len);

/* The file's len bytes at offset, mapped into memory; NULL if they can't be. */
typedef const char *(*ProllyTableFileMap) (void *arg, uint64 offset, Size len);

typedef struct ProllyTableFileRoutine
{
	ProllyImplInfo info;

	ProllyTableFileWriter *(*write_begin) (MemoryContext cxt);
	void		(*write_add) (ProllyTableFileWriter *w, const ProllyHash *hash,
							  const void *data, Size len, StringInfo out);
	void		(*write_finish) (ProllyTableFileWriter *w, StringInfo out);

	ProllyTableFile *(*open) (ProllyTableFileRead read, ProllyTableFileMap map,
							  void *arg, uint64 size, MemoryContext cxt);
	bool		(*find) (ProllyTableFile *tf, const ProllyHash *hash,
						 uint64 *offset, uint32 *len);
	void		(*decode) (ProllyTableFile *tf, const char *bytes, uint32 len,
						   StringInfo out);
	int			(*count) (ProllyTableFile *tf);
	void		(*entry) (ProllyTableFile *tf, int i, ProllyHash *hash,
						  uint64 *offset, uint32 *len);
	void		(*close) (ProllyTableFile *tf);
	int			(*locate) (ProllyTableFile *tf, const ProllyHash *hash);

	/* Optional. */
	uint32		(*version) (ProllyTableFileRead read, void *arg, uint64 size);
} ProllyTableFileRoutine;

#define PROLLY_TABLEFILE_CALLBACK_NAMES \
	"write_begin", "write_add", "write_finish", "open", "find", "decode", \
	"count", "entry", "close", "locate", "version"
#define PROLLY_TABLEFILE_FIRST_CALLBACK write_begin
#define PROLLY_TABLEFILE_LAST_CALLBACK version

static inline const ProllyTableFileRoutine *
prolly_tablefile_lookup(const char *name)
{
	return (const ProllyTableFileRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_FILE, name);
}

#endif							/* PROLLY_TABLEFILE_H */
