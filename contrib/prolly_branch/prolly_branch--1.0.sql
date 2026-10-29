/* contrib/prolly_branch/prolly_branch--1.0.sql */

\echo Use "CREATE EXTENSION prolly_branch" to load this file. \quit

CREATE FUNCTION copy_files(rel regclass) RETURNS oid
AS 'MODULE_PATHNAME', 'prolly_branch_copy_files' LANGUAGE C STRICT;

CREATE FUNCTION point(rel regclass, filenode oid, relpages int4, reltuples float4,
					  relallvisible int4, relfrozenxid xid, relminmxid xid) RETURNS void
AS 'MODULE_PATHNAME', 'prolly_branch_point' LANGUAGE C STRICT;

CREATE FUNCTION drop_file(rel regclass, filenode oid) RETURNS void
AS 'MODULE_PATHNAME', 'prolly_branch_drop_file' LANGUAGE C STRICT;

-- Each branch's file for each relation, with the pg_class fields that
-- describe the file.  The checked-out branch's rows are refreshed from
-- pg_class whenever it is left or copied.
CREATE TABLE files (
	branch text,
	relid oid,
	filenode oid NOT NULL,
	relpages int4 NOT NULL,
	reltuples float4 NOT NULL,
	relallvisible int4 NOT NULL,
	relfrozenxid xid NOT NULL,
	relminmxid xid NOT NULL,
	PRIMARY KEY (branch, relid)
);

CREATE TABLE checked_out (
	nsp oid PRIMARY KEY,
	branch text NOT NULL
);

-- Every relation of the schema with files: tables, materialized views,
-- sequences, their indexes, and their TOAST tables and TOAST indexes.
CREATE FUNCTION rels(nsp oid) RETURNS SETOF oid LANGUAGE sql STABLE AS $$
	SELECT c.oid FROM pg_class c
	 WHERE c.relnamespace = nsp AND c.relkind IN ('r', 'm', 'S', 'i')
	UNION
	SELECT c.reltoastrelid FROM pg_class c
	 WHERE c.relnamespace = nsp AND c.reltoastrelid <> 0
	UNION
	SELECT i.indexrelid FROM pg_class c JOIN pg_index i ON i.indrelid = c.reltoastrelid
	 WHERE c.relnamespace = nsp AND c.reltoastrelid <> 0
$$;

CREATE FUNCTION init(sch name) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	n oid := sch::regnamespace;
BEGIN
	INSERT INTO prolly_branch.checked_out VALUES (n, 'main');
	PERFORM prolly_branch.remember(n);
END $$;

-- The checked-out branch's files as pg_class has them now.
CREATE FUNCTION remember(nsp oid) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	b text := (SELECT branch FROM prolly_branch.checked_out WHERE checked_out.nsp = remember.nsp);
BEGIN
	INSERT INTO prolly_branch.files
	SELECT b, c.oid, c.relfilenode, c.relpages, c.reltuples, c.relallvisible,
		   c.relfrozenxid, c.relminmxid
	  FROM pg_class c WHERE c.oid IN (SELECT prolly_branch.rels(nsp))
	ON CONFLICT (branch, relid) DO UPDATE SET
		filenode = EXCLUDED.filenode, relpages = EXCLUDED.relpages,
		reltuples = EXCLUDED.reltuples, relallvisible = EXCLUDED.relallvisible,
		relfrozenxid = EXCLUDED.relfrozenxid, relminmxid = EXCLUDED.relminmxid;
END $$;

-- A new branch: a copy of every file of the checked-out branch.
CREATE FUNCTION create_branch(sch name, name text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	n oid := sch::regnamespace;
	r record;
BEGIN
	PERFORM prolly_branch.remember(n);
	FOR r IN SELECT f.* FROM prolly_branch.files f
			  JOIN prolly_branch.checked_out o ON o.nsp = n
				AND f.branch = o.branch
			 WHERE f.relid IN (SELECT prolly_branch.rels(n)) LOOP
		INSERT INTO prolly_branch.files VALUES (name, r.relid,
			prolly_branch.copy_files(r.relid), r.relpages, r.reltuples, r.relallvisible,
			r.relfrozenxid, r.relminmxid);
	END LOOP;
END $$;

-- Points every relation of the schema at the branch's files.
CREATE FUNCTION checkout(sch name, name text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	n oid := sch::regnamespace;
	r record;
	f prolly_branch.files;
BEGIN
	IF name = (SELECT branch FROM prolly_branch.checked_out o WHERE o.nsp = n) THEN
		RETURN;
	END IF;
	PERFORM prolly_branch.remember(n);
	FOR r IN SELECT prolly_branch.rels(n) AS relid LOOP
		SELECT * INTO f FROM prolly_branch.files WHERE branch = name AND relid = r.relid;
		IF NOT FOUND THEN
			RAISE EXCEPTION 'branch % has no file for %: definitions differ', name, r.relid::regclass;
		END IF;
		PERFORM prolly_branch.point(f.relid, f.filenode, f.relpages, f.reltuples,
									f.relallvisible, f.relfrozenxid, f.relminmxid);
	END LOOP;
	UPDATE prolly_branch.checked_out o SET branch = name WHERE o.nsp = n;
END $$;

CREATE FUNCTION drop_branch(sch name, name text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	n oid := sch::regnamespace;
	f prolly_branch.files;
BEGIN
	IF name = (SELECT branch FROM prolly_branch.checked_out o WHERE o.nsp = n) THEN
		RAISE EXCEPTION 'branch % is checked out', name;
	END IF;
	FOR f IN SELECT * FROM prolly_branch.files
			  WHERE branch = name AND relid IN (SELECT prolly_branch.rels(n)) LOOP
		PERFORM prolly_branch.drop_file(f.relid, f.filenode);
	END LOOP;
	DELETE FROM prolly_branch.files
	 WHERE branch = name AND relid IN (SELECT prolly_branch.rels(n));
END $$;
