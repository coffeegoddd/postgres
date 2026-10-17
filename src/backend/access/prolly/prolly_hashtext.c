/*-------------------------------------------------------------------------
 *
 * prolly_hashtext.c
 *	  Content hashes as text: how commit ids, chunk addresses and remote
 *	  manifests write them, and read them back.
 *
 * A hash is written as lowercase hex, two digits a byte, whatever its
 * length; reading takes either case.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_hashtext.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/prolly_types.h"
#include "utils/builtins.h"

char *
prolly_hash_hex(const ProllyHash *hash)
{
	char	   *hex = palloc(hash->len * 2 + 1);

	hex_encode((const char *) hash->bytes, hash->len, hex);
	hex[hash->len * 2] = '\0';
	return hex;
}

bool
prolly_hash_from_hex(const char *hex, size_t len, ProllyHash *hash)
{
	if (len == 0 || len % 2 != 0 || len / 2 > PROLLY_HASH_MAX_LEN)
		return false;
	for (size_t i = 0; i < len; i++)
	{
		if (!isxdigit((unsigned char) hex[i]))
			return false;
	}
	memset(hash, 0, sizeof(*hash));
	hash->len = len / 2;
	(void) hex_decode(hex, len, (char *) hash->bytes);
	return true;
}
