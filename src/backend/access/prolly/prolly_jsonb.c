/*-------------------------------------------------------------------------
 *
 * prolly_jsonb.c
 *	  SQL entry points that work on a jsonb document's JSON tree
 *	  (prolly_jsontree.h) when the document is stored as one.
 *
 * PostgreSQL's jsonb functions take a document whole: a detoast reads
 * every item of a JSON tree.  These take the value as it is stored, and
 * when it is a hash pointer to a JSON tree (prolly_toast.h), they read
 * only the items under a path, set a value by editing the tree, or diff
 * two trees by their nodes.  Otherwise, and for what a tree doesn't
 * handle, they read the document whole and do what PostgreSQL's
 * functions do, so their results never depend on how a value is stored:
 *
 *	prolly_jsonb_get(doc, path)		doc #> path
 *	prolly_jsonb_set(doc, path, value, create)
 *									jsonb_set(doc, path, value, create)
 *	prolly_jsonb_diff(a, b)			each scalar path whose value differs
 *
 * jsonb's own ->, ->>, #>, #>>, ? and jsonb_set() take a tree's path the
 * same way, through prolly_jsonb_tree_get() and prolly_jsonb_tree_set()
 * (prolly_jsonb.h), so prolly_jsonb_set() is jsonb_set() itself.
 *
 * An edit on a JSON tree returns a hash pointer to the new tree, which a
 * versioned table's row takes as it is: an UPDATE writes the nodes along
 * the path, not the document.  (The pointer's sizes are an estimate;
 * whether a document is a tree never depends on them.)  A version of a
 * row read from history names its tree too, so diffing two versions by
 * prolly_jsonb_diff() reads only what differs.  In a database whose
 * encoding isn't UTF-8 they all read documents whole.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_jsonb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_jsonb.h"
#include "access/prolly_jsontree.h"
#include "access/prolly_toast.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"
#include "varatt.h"

static const ProllyJsonTreeRoutine *
json_tree(void)
{
	static const ProllyJsonTreeRoutine *jt = NULL;

	if (jt == NULL)
		jt = prolly_jsontree_lookup("paths");
	return jt;
}

/* path's elements as strings; false if one is NULL. */
static bool
path_elements(ArrayType *path, char ***elems, int *n)
{
	Datum	   *datums;
	bool	   *nulls;

	deconstruct_array_builtin(path, TEXTOID, &datums, &nulls, n);
	*elems = palloc(sizeof(char *) * Max(*n, 1));
	for (int i = 0; i < *n; i++)
	{
		if (nulls[i])
			return false;
		(*elems)[i] = TextDatumGetCString(datums[i]);
	}
	return true;
}

/*
 * A jsonb value as stored: a JSON tree's root, if it is one.  A tree's
 * paths and strings are in UTF-8, so in another encoding the document is
 * read whole.
 */
static bool
datum_tree(Datum value, ProllyNodeRef *root, Size *rawsize)
{
	varlena    *v = (varlena *) DatumGetPointer(value);

	/* (an ordinary value, in memory or toasted, is told apart unread) */
	if (!VARATT_IS_EXTERNAL_ONDISK_HASH(v))
		return false;
	return GetDatabaseEncoding() == PG_UTF8 &&
		prolly_toast_json_tree(v, root, rawsize);
}

static bool
arg_tree(FunctionCallInfo fcinfo, int argno, ProllyNodeRef *root,
		 Size *rawsize)
{
	return datum_tree(PG_GETARG_DATUM(argno), root, rawsize);
}

bool
prolly_jsonb_tree_kind(Datum doc, enum jbvType *kind)
{
	ProllyNodeRef root;
	Size		rawsize;

	if (!datum_tree(doc, &root, &rawsize))
		return false;
	*kind = PROLLY_CALL(json_tree(), kind, prolly_toast_env(), &root);
	return true;
}

bool
prolly_jsonb_tree_get(Datum doc, ProllyJsonbKind kind, char **path,
					  int npath, Jsonb **found)
{
	ProllyNodeRef root;
	Size		rawsize;

	if (!datum_tree(doc, &root, &rawsize))
		return false;
	if (kind != PROLLY_JSONB_ANY)
	{
		enum jbvType is = PROLLY_CALL(json_tree(), kind, prolly_toast_env(),
									  &root);

		if (is != (kind == PROLLY_JSONB_OBJECT ? jbvObject : jbvArray))
		{
			*found = NULL;
			return true;
		}
	}
	return PROLLY_CALL(json_tree(), get, prolly_toast_env(), &root, path,
					   npath, found);
}

bool
prolly_jsonb_tree_set(Datum doc, ArrayType *patharr, Jsonb *value,
					  bool create, Datum *result)
{
	ProllyNodeRef root;
	ProllyNodeRef new_root;
	Size		rawsize;
	char	  **path;
	int			npath;
	int64		delta;
	Jsonb	   *whole;

	/* a path of more than one dimension, or with a NULL step, is an error */
	if (ARR_NDIM(patharr) > 1 ||
		!datum_tree(doc, &root, &rawsize) ||
		!path_elements(patharr, &path, &npath))
		return false;
	if (!PROLLY_CALL(json_tree(), set, prolly_toast_env(), &root, path,
					 npath, value, create, &new_root, &delta, &whole))
		return false;
	/* a document too short to be a tree is itself */
	if (whole != NULL)
		*result = JsonbPGetDatum(whole);
	else if (new_root.hash.len == root.hash.len &&
			 memcmp(new_root.hash.bytes, root.hash.bytes, root.hash.len) == 0)
		*result = doc;
	else
		*result = PointerGetDatum(prolly_toast_json_pointer(&new_root,
															(Size) Max((int64) rawsize + delta,
																	   VARHDRSZ + 1)));
	return true;
}

Datum
prolly_jsonb_get(PG_FUNCTION_ARGS)
{
	ArrayType  *patharr = PG_GETARG_ARRAYTYPE_P(1);
	char	  **path;
	int			npath;
	Datum	   *datums;
	bool	   *nulls;
	bool		isnull;
	Datum		result;
	Jsonb	   *found;

	if (path_elements(patharr, &path, &npath) &&
		prolly_jsonb_tree_get(PG_GETARG_DATUM(0), PROLLY_JSONB_ANY, path,
							  npath, &found))
	{
		if (found == NULL)
			PG_RETURN_NULL();
		PG_RETURN_JSONB_P(found);
	}

	/* as #> does it */
	deconstruct_array_builtin(patharr, TEXTOID, &datums, &nulls, &npath);
	for (int i = 0; i < npath; i++)
		if (nulls[i])
			PG_RETURN_NULL();
	result = jsonb_get_element(PG_GETARG_JSONB_P(0), datums, npath, &isnull,
							   false);
	if (isnull)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/* jsonb_set() takes the tree's path itself */
Datum
prolly_jsonb_set(PG_FUNCTION_ARGS)
{
	return jsonb_set(fcinfo);
}

typedef struct DiffOut
{
	ReturnSetInfo *rsinfo;
} DiffOut;

static void
diff_row(ProllyDiffKind kind, char **path, int npath, Jsonb *before,
		 Jsonb *after, void *arg)
{
	DiffOut    *out = arg;
	Datum	   *elems = palloc(sizeof(Datum) * Max(npath, 1));
	Datum		values[4];
	bool		nulls[4] = {false, false, before == NULL, after == NULL};

	for (int i = 0; i < npath; i++)
		elems[i] = CStringGetTextDatum(path[i]);
	values[0] = PointerGetDatum(construct_array_builtin(elems, npath, TEXTOID));
	values[1] = CStringGetTextDatum(kind == PROLLY_DIFF_ADDED ? "added" :
									kind == PROLLY_DIFF_REMOVED ? "removed" :
									"modified");
	values[2] = before ? JsonbPGetDatum(before) : (Datum) 0;
	values[3] = after ? JsonbPGetDatum(after) : (Datum) 0;
	tuplestore_putvalues(out->rsinfo->setResult, out->rsinfo->setDesc,
						 values, nulls);
}

Datum
prolly_jsonb_diff(PG_FUNCTION_ARGS)
{
	ProllyNodeRef a;
	ProllyNodeRef b;
	Size		rawsize;
	DiffOut		out;

	InitMaterializedSRF(fcinfo, 0);
	out.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	if (arg_tree(fcinfo, 0, &a, &rawsize) && arg_tree(fcinfo, 1, &b, &rawsize))
		PROLLY_CALL(json_tree(), diff, prolly_toast_env(), &a, &b, diff_row,
					&out);
	else
		PROLLY_CALL(json_tree(), diff_documents, PG_GETARG_JSONB_P(0),
					PG_GETARG_JSONB_P(1), diff_row, &out);
	return (Datum) 0;
}

Datum
prolly_jsonb_tree_root(PG_FUNCTION_ARGS)
{
	ProllyNodeRef root;
	Size		rawsize;

	if (!arg_tree(fcinfo, 0, &root, &rawsize))
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(prolly_hash_hex(&root.hash)));
}
