-- Moving a branch made by copying to a commit that defines its objects
-- differently: the definitions change first, then the rows are written,
-- then what the rows must satisfy is added.  After each move the branch
-- folds to the commit's root.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.p (id int PRIMARY KEY, name text);
CREATE TABLE app.t (id int PRIMARY KEY, v text, gone int);
INSERT INTO app.p VALUES (1, 'one'), (2, 'two');
INSERT INTO app.t SELECT g, 'v' || g, g FROM generate_series(1, 20) g;
SELECT (dolt_commit('-Am', 'base'))[1] AS base \gset
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');

-- Everything kind of change.
CREATE TYPE app.mood AS ENUM ('sad', 'happy');
CREATE SEQUENCE app.counter START 100;
CREATE FUNCTION app.stamp() RETURNS trigger LANGUAGE plpgsql AS
$$BEGIN NEW.note := coalesce(NEW.note, 'stamped'); RETURN NEW; END$$;
CREATE TABLE app.c (id int PRIMARY KEY, p int NOT NULL REFERENCES app.p,
                    mood app.mood DEFAULT 'happy', n int DEFAULT nextval('app.counter'),
                    note text CHECK (note <> ''), made int GENERATED ALWAYS AS (id * 2) STORED);
CREATE INDEX c_p ON app.c (p);
CREATE TRIGGER c_stamp BEFORE INSERT ON app.c FOR EACH ROW EXECUTE FUNCTION app.stamp();
INSERT INTO app.c (id, p) VALUES (1, 1), (2, 2), (3, 1);
ALTER TABLE app.t ADD COLUMN extra int DEFAULT 7;
ALTER TABLE app.t DROP COLUMN gone;
ALTER TABLE app.t ALTER COLUMN v TYPE varchar(20);
CREATE UNIQUE INDEX t_v ON app.t (v);
UPDATE app.t SET extra = id WHERE id <= 5;
CREATE VIEW app.c_view AS SELECT c.id, p.name FROM app.c c JOIN app.p p ON p.id = c.p;
SELECT (dolt_commit('-Am', 'feature'))[1] AS feature \gset

-- main fast-forwards to it.
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature'))[2:4];
SELECT test_fold_root() = prolly_root_hash('feature') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM app.c ORDER BY id;
SELECT * FROM app.c_view ORDER BY id;
SELECT id, v, extra FROM app.t WHERE id IN (1, 6) ORDER BY id;
SELECT nextval('app.counter') >= 100 AS sequence_made;
INSERT INTO app.c (id, p) VALUES (4, 2);
SELECT note, made FROM app.c WHERE id = 4;		-- the trigger and generation
INSERT INTO app.c (id, p) VALUES (5, 99);		-- the foreign key
INSERT INTO app.t (id, v) VALUES (100, 'v1');	-- the unique index
SELECT dolt_reset('--hard');

-- And back: everything feature made is gone.
SELECT dolt_reset('--hard', :'base');
SELECT test_fold_root() = prolly_root_hash(:'base') AS same_root;
SELECT to_regclass('app.c') IS NULL AS no_c, to_regclass('app.c_view') IS NULL AS no_view,
       to_regtype('app.mood') IS NULL AS no_type, to_regclass('app.counter') IS NULL AS no_sequence,
       to_regprocedure('app.stamp()') IS NULL AS no_function;
SELECT * FROM app.t WHERE id = 1;

-- Columns in another order: the table is made again, and so are the
-- foreign key into it and the view on it.
SELECT dolt_checkout('feature');
DROP VIEW app.c_view;
ALTER TABLE app.p DROP COLUMN name;
ALTER TABLE app.p ADD COLUMN early int;
ALTER TABLE app.p ADD COLUMN name text;
UPDATE app.p SET name = 'p' || id;
CREATE VIEW app.c_view AS SELECT c.id, p.name FROM app.c c JOIN app.p p ON p.id = c.p;
CREATE VIEW app.a_view AS SELECT count(*) AS n FROM app.c_view;	-- before it by name
SELECT (dolt_commit('-Am', 'reordered'))[1] AS reordered \gset
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard', :'feature');		-- c and its foreign key into p
SELECT dolt_reset('--hard', :'reordered');
SELECT test_fold_root() = prolly_root_hash(:'reordered') AS same_root;
SELECT * FROM app.p ORDER BY id;
SELECT * FROM app.a_view;
INSERT INTO app.c (id, p) VALUES (9, 99);		-- the foreign key, again

-- A branch at a commit defined differently from the session's branch.
SELECT dolt_reset('--hard', :'base');
SELECT dolt_branch('at_feature', :'feature');
SELECT dolt_checkout('at_feature');
SELECT test_fold_root() = prolly_root_hash(:'feature') AS same_root;
SELECT count(*) FROM app.c;

SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'feature', 'at_feature');

-- Versioned schemas come and go with a move, empty ones too.
SELECT test_fold_root() AS no_schemas \gset
SELECT dolt_hashof('HEAD') AS before_schemas \gset
CREATE SCHEMA sm_full VERSIONED;
CREATE TABLE sm_full.u (id int PRIMARY KEY);
INSERT INTO sm_full.u VALUES (1);
CREATE SCHEMA sm_empty VERSIONED;
SELECT dolt_commit('-Am', 'schemas') AS with_schemas \gset
SELECT dolt_reset('--hard', :'before_schemas');
SELECT nspname FROM pg_namespace WHERE nspname LIKE 'sm\_%' ORDER BY 1;
SELECT test_fold_root() = :'no_schemas' AS schemas_gone;
SELECT dolt_reset('--hard', (:'with_schemas'::text[])[1]);
SELECT nspname FROM pg_namespace WHERE nspname LIKE 'sm\_%' ORDER BY 1;
SELECT * FROM sm_full.u;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS schemas_back;

-- Changes that need others made first, or that take their own along: a
-- column of a type, sequence or function made in the same move; a column
-- retyped whose old default won't cast; a NOT NULL, indexed column
-- dropped; a serial table made again; a NOT NULL domain; a key changed in
-- place under a foreign key; a function's result type; an identity column.
CREATE SCHEMA sm7 VERSIONED;
CREATE TABLE sm7.a (id int PRIMARY KEY, s text DEFAULT 'x', g int NOT NULL);
CREATE INDEX a_g ON sm7.a (g);
INSERT INTO sm7.a VALUES (1, 'happy', 1);
CREATE TABLE sm7.p (id serial PRIMARY KEY, name text);
INSERT INTO sm7.p (name) VALUES ('one');
CREATE TABLE sm7.k (id int PRIMARY KEY);
CREATE TABLE sm7.r (id int PRIMARY KEY, k int REFERENCES sm7.k);
INSERT INTO sm7.k VALUES (1);
INSERT INTO sm7.r VALUES (1, 1);
CREATE FUNCTION sm7.f(int) RETURNS int LANGUAGE sql AS 'SELECT $1';
SELECT (dolt_commit('-Am', 'sm7 base'))[1] AS sm7_base \gset
SELECT dolt_branch('sm7_feature');
SELECT dolt_checkout('sm7_feature');
CREATE TYPE sm7.mood AS ENUM ('sad', 'happy');
CREATE SEQUENCE sm7.seq;
CREATE FUNCTION sm7.five() RETURNS int LANGUAGE sql AS 'SELECT 5';
ALTER TABLE sm7.a ADD COLUMN m sm7.mood;
ALTER TABLE sm7.a ADD COLUMN n int DEFAULT nextval('sm7.seq');
ALTER TABLE sm7.a ADD COLUMN q int DEFAULT sm7.five();
ALTER TABLE sm7.a ALTER COLUMN s DROP DEFAULT;
ALTER TABLE sm7.a ALTER COLUMN s TYPE sm7.mood USING s::sm7.mood;
ALTER TABLE sm7.a ALTER COLUMN s SET DEFAULT 'happy';
ALTER TABLE sm7.a DROP COLUMN g;
DROP TABLE sm7.p;
CREATE TABLE sm7.p (name text, id serial PRIMARY KEY);
INSERT INTO sm7.p (name) VALUES ('again');
CREATE DOMAIN sm7.pos AS int NOT NULL;
CREATE TABLE sm7.d (v sm7.pos);
ALTER TABLE sm7.k RENAME CONSTRAINT k_pkey TO k_key;
DROP FUNCTION sm7.f(int);
CREATE FUNCTION sm7.f(int) RETURNS text LANGUAGE sql AS 'SELECT $1::text';
CREATE TABLE sm7.i (id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v text);
INSERT INTO sm7.i (v) VALUES ('a');
SELECT (dolt_commit('-Am', 'sm7 feature'))[1] AS sm7_feature \gset
SELECT dolt_checkout('main');
-- (sm7.p, dropped and made again on the feature, is another table there:
-- a table identity conflict, resolved with the feature's)
BEGIN;
SELECT (dolt_merge('sm7_feature'))[2:4];
SELECT table_name FROM dolt_identity_conflicts();
SELECT dolt_conflicts_resolve('--theirs', 'sm7.p');
SELECT length(dolt_commit('-m', 'sm7 feature merged')::text) AS committed;
COMMIT;
SELECT test_fold_root() = prolly_root_hash('main') AS same_root;
SELECT * FROM sm7.a;
SELECT * FROM sm7.p;
SELECT nextval('sm7.p_id_seq') > 1 AS serial_kept, pg_get_serial_sequence('sm7.p', 'id');
SELECT * FROM sm7.i ORDER BY id;
SELECT count(*) AS identity_sequences FROM pg_class
WHERE relnamespace = 'sm7'::regnamespace AND relname LIKE 'i_id_seq%';
SELECT sm7.f(3);
SELECT dolt_reset('--hard');
-- (the base's sm7.p is another table: dropped here first, the reset makes
-- it again)
DROP TABLE sm7.p;
SELECT dolt_reset('--hard', :'sm7_base');
SELECT test_fold_root() = prolly_root_hash(:'sm7_base') AS same_root;
SELECT * FROM sm7.a;
SELECT sm7.f(3);
SELECT conname FROM pg_constraint WHERE conrelid = 'sm7.k'::regclass;
SELECT dolt_branch('-D', 'sm7_feature');
DROP SCHEMA sm7 CASCADE;
SELECT length(dolt_commit('-Am', 'without sm7')::text) AS committed;

-- A view's unchanged dependents aren't lost with it: a materialized view
-- over it is made again, with the rows the commit holds.
CREATE SCHEMA sm8 VERSIONED;
CREATE TABLE sm8.t (id int PRIMARY KEY);
CREATE VIEW sm8.v AS SELECT id FROM sm8.t;
CREATE MATERIALIZED VIEW sm8.mv AS SELECT id FROM sm8.v;
SELECT (dolt_commit('-Am', 'sm8 base'))[1] AS sm8_base \gset
CREATE TABLE sm8.u (id int);
SELECT length(dolt_commit('-Am', 'sm8 more')::text) AS committed;
SELECT dolt_reset('--hard', :'sm8_base');
SELECT to_regclass('sm8.mv') IS NOT NULL AS mv_kept;
SELECT test_fold_root() = prolly_root_hash(:'sm8_base') AS same_root;
DROP SCHEMA sm8 CASCADE;
SELECT length(dolt_commit('-Am', 'without sm8')::text) AS committed;

-- A move to a commit whose column is NOT NULL, from a branch with NULLs in
-- it: the NOT NULL is made once the commit's rows are written (the NULLs
-- gone with them).  A primary key's columns keep theirs before.
CREATE SCHEMA sm9 VERSIONED;
CREATE TABLE sm9.t (pk int PRIMARY KEY, col1 int NOT NULL);
INSERT INTO sm9.t VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'sm9 not null')::text) AS committed;
SELECT dolt_branch('sm9_right');
ALTER TABLE sm9.t ALTER COLUMN col1 DROP NOT NULL;
INSERT INTO sm9.t VALUES (3, NULL);
SELECT length(dolt_commit('-Am', 'sm9 nullable')::text) AS committed;
SELECT dolt_checkout('sm9_right');
SELECT * FROM sm9.t ORDER BY pk;
SELECT attnotnull FROM pg_attribute WHERE attrelid = 'sm9.t'::regclass AND attname = 'col1';
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'sm9_right');
DROP SCHEMA sm9 CASCADE;
SELECT length(dolt_commit('-Am', 'without sm9')::text) AS committed;

-- Expressions Postgres doesn't print as they read back (an IN list over a
-- varchar prints with its array's cast, read back as a cast of each
-- element) are described as they read back: a branch moved by making them
-- again from their text is defined as the commit is.  sm10_right is first
-- used after main changed each of them, so its move makes them again.
CREATE SCHEMA sm10 VERSIONED;
CREATE DOMAIN sm10.d AS varchar(10) CHECK (VALUE IN ('a', 'b'));
CREATE TABLE sm10.t (pk int PRIMARY KEY, c1 varchar(10) CHECK (c1 IN ('a', 'b')),
	g bool GENERATED ALWAYS AS (c1 IN ('x', 'y')) STORED,
	f bool DEFAULT ('q'::varchar IN ('a', 'b')), d sm10.d);
CREATE POLICY p ON sm10.t USING (c1 IN ('a', 'b'));
INSERT INTO sm10.t (pk, c1) VALUES (1, 'a');
SELECT (dolt_commit('-Am', 'sm10 base'))[1] AS sm10_base \gset
SELECT dolt_branch('sm10_right');
ALTER DOMAIN sm10.d DROP CONSTRAINT d_check;
ALTER DOMAIN sm10.d ADD CONSTRAINT d_check CHECK (VALUE IN ('a', 'b', 'c'));
ALTER TABLE sm10.t DROP CONSTRAINT t_c1_check, ALTER COLUMN f SET DEFAULT true,
	ADD CONSTRAINT t_c1_check CHECK (c1 IN ('a', 'b', 'c'));
ALTER TABLE sm10.t ALTER COLUMN g SET EXPRESSION AS (c1 IN ('x', 'y', 'z'));
DROP POLICY p ON sm10.t;
CREATE POLICY p ON sm10.t USING (c1 IN ('a', 'b', 'c'));
SELECT length(dolt_commit('-Am', 'sm10 main')::text) AS committed;
SELECT dolt_checkout('sm10_right');
SELECT test_fold_root() = prolly_root_hash(:'sm10_base') AS same_root;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_checkout('main');
-- a commit of the format before (its expressions described as printed,
-- which a fold now doesn't match) isn't moved to
SELECT test_commit_of(test_root_with_format(prolly_root_hash(:'sm10_base'), '5'),
	'format 5') AS sm10_old \gset
SELECT dolt_reset('--hard', :'sm10_old');
SELECT dolt_branch('-D', 'sm10_right');
DROP SCHEMA sm10 CASCADE;
SELECT length(dolt_commit('-Am', 'without sm10')::text) AS committed;

-- A move to a commit whose column has another type, from rows that don't
-- cast to it (or cast to values its unique index refuses): every row is
-- the commit's, so the branch's go first, with no trigger fired and no
-- foreign key's action taken, and the table is kept (its privileges and
-- comment with it).
CREATE SCHEMA sm11 VERSIONED;
CREATE TABLE sm11.log (n text);
CREATE TABLE sm11.p (pk int PRIMARY KEY, col1 int UNIQUE);
CREATE TABLE sm11.c (id int PRIMARY KEY, p int REFERENCES sm11.p ON DELETE CASCADE);
CREATE FUNCTION sm11.f() RETURNS trigger LANGUAGE plpgsql
	AS $$BEGIN INSERT INTO sm11.log VALUES ('fired'); RETURN OLD; END$$;
CREATE TRIGGER tr BEFORE DELETE ON sm11.p FOR EACH ROW EXECUTE FUNCTION sm11.f();
INSERT INTO sm11.p VALUES (1, 10), (2, 20);
INSERT INTO sm11.c VALUES (1, 1), (2, 2);
GRANT SELECT ON sm11.p TO PUBLIC;
COMMENT ON TABLE sm11.p IS 'the p';
SELECT 'sm11.p'::regclass::oid AS sm11_p \gset
SELECT length(dolt_commit('-Am', 'sm11 base')::text) AS committed;
SELECT dolt_branch('sm11_right');
ALTER TABLE sm11.p ALTER COLUMN col1 TYPE varchar(100);
INSERT INTO sm11.p VALUES (3, 'thirty'), (4, '03');
UPDATE sm11.p SET col1 = '3' WHERE pk = 2;
SELECT length(dolt_commit('-Am', 'sm11 main')::text) AS committed;
SELECT dolt_checkout('sm11_right');
SELECT * FROM sm11.p ORDER BY pk;
SELECT * FROM sm11.c ORDER BY id;
SELECT count(*) AS fired FROM sm11.log;
SELECT format_type(atttypid, atttypmod) FROM pg_attribute
WHERE attrelid = 'sm11.p'::regclass AND attname = 'col1';
SELECT 'sm11.p'::regclass::oid = :sm11_p AS same_table,
	has_table_privilege('public', 'sm11.p', 'SELECT') AS granted,
	obj_description('sm11.p'::regclass, 'pg_class') AS comment;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_checkout('main');
SELECT * FROM sm11.p ORDER BY pk;
SELECT dolt_branch('-D', 'sm11_right');
DROP SCHEMA sm11 CASCADE;
SELECT length(dolt_commit('-Am', 'without sm11')::text) AS committed;

-- A table emptied for its key's change of type, that another's foreign key
-- references: the key goes before the rows do and comes back after, so
-- the other's rows aren't checked against an empty table.  A trigger
-- enabled ALWAYS fires (warns) for the rows emptied no more than for the
-- rows written.  A column of a type the commit defines otherwise is emptied
-- too, rather than converted to what its NOT NULL refuses; a key of one
-- (a domain made over another base type), that another table's foreign
-- key references, loses that key before it converts: so do those of a
-- table the move drops (kd, and a partition, kq2, its own) or makes again
-- (kr), and one whose definition changes (kf).
CREATE SCHEMA sm12 VERSIONED;
CREATE DOMAIN sm12.pos AS int CHECK (VALUE > 0);
CREATE TABLE sm12.p (id int PRIMARY KEY);
CREATE TABLE sm12.c (id int PRIMARY KEY, pid int REFERENCES sm12.p);
CREATE TABLE sm12.d (id int PRIMARY KEY, v sm12.pos NOT NULL);
CREATE DOMAIN sm12.k AS int CHECK (VALUE > 0);
CREATE TABLE sm12.kp (k sm12.k PRIMARY KEY);
CREATE TABLE sm12.kc (id int PRIMARY KEY, pid int REFERENCES sm12.kp);
CREATE TABLE sm12.kr (id int PRIMARY KEY, pid int REFERENCES sm12.kp);
CREATE TABLE sm12.kf (id int PRIMARY KEY, pid int REFERENCES sm12.kp);
CREATE TABLE sm12.kq (id int, pid int) PARTITION BY RANGE (id);
CREATE TABLE sm12.kq1 PARTITION OF sm12.kq FOR VALUES FROM (0) TO (10);
CREATE FUNCTION sm12.f() RETURNS trigger LANGUAGE plpgsql
	AS $$BEGIN RAISE WARNING 'fired'; RETURN OLD; END$$;
CREATE TRIGGER tr BEFORE DELETE ON sm12.p FOR EACH ROW EXECUTE FUNCTION sm12.f();
ALTER TABLE sm12.p ENABLE ALWAYS TRIGGER tr;
INSERT INTO sm12.p VALUES (1), (2);
INSERT INTO sm12.c VALUES (1, 1), (2, 2);
INSERT INTO sm12.d VALUES (1, 5);
INSERT INTO sm12.kp VALUES (1), (2);
INSERT INTO sm12.kc VALUES (1, 1), (2, 2);
INSERT INTO sm12.kr VALUES (1, 1), (2, 2);
INSERT INTO sm12.kf VALUES (1, 1), (2, 2);
INSERT INTO sm12.kq VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'sm12 base')::text) AS committed;
SELECT dolt_branch('sm12_right');
ALTER TABLE sm12.p ALTER COLUMN id TYPE bigint;
UPDATE sm12.d SET v = 20;
ALTER DOMAIN sm12.pos DROP CONSTRAINT pos_check;
ALTER DOMAIN sm12.pos ADD CONSTRAINT pos_check CHECK (VALUE > 10);
CREATE TABLE sm12.kd (id int PRIMARY KEY, pid int REFERENCES sm12.kp);
INSERT INTO sm12.kd VALUES (1, 1), (2, 2);
CREATE TABLE sm12.kq2 PARTITION OF sm12.kq FOR VALUES FROM (10) TO (20);
ALTER TABLE sm12.kq2 ADD CONSTRAINT kq2_fk FOREIGN KEY (pid) REFERENCES sm12.kp;
INSERT INTO sm12.kq VALUES (11, 2);
DROP TABLE sm12.kr;
CREATE TABLE sm12.kr (pid int REFERENCES sm12.kp, id int PRIMARY KEY);
INSERT INTO sm12.kr VALUES (1, 1), (2, 2);
ALTER TABLE sm12.kf DROP CONSTRAINT kf_pid_fkey,
	ADD CONSTRAINT kf_pid_fkey FOREIGN KEY (pid) REFERENCES sm12.kp ON DELETE CASCADE;
ALTER TABLE sm12.kp ALTER COLUMN k TYPE int;
DROP DOMAIN sm12.k;
CREATE DOMAIN sm12.k AS bigint CHECK (VALUE > 0);
ALTER TABLE sm12.kp ALTER COLUMN k TYPE sm12.k;
SELECT (dolt_commit('-Am', 'sm12 main'))[1] AS sm12_main \gset
SELECT dolt_checkout('sm12_right');
SELECT format_type(atttypid, atttypmod) FROM pg_attribute
WHERE attrelid = 'sm12.p'::regclass AND attname = 'id';
SELECT * FROM sm12.c ORDER BY id;
SELECT conname FROM pg_constraint WHERE conrelid = 'sm12.c'::regclass
  AND contype = 'f';
SELECT format_type(typbasetype, typtypmod) FROM pg_type WHERE oid = 'sm12.k'::regtype;
SELECT * FROM sm12.kc ORDER BY id;
SELECT conrelid::regclass, pg_get_constraintdef(oid) FROM pg_constraint
WHERE confrelid = 'sm12.kp'::regclass ORDER BY conrelid::regclass::text;
SELECT to_regclass('sm12.kd') AS kd, (SELECT count(*) FROM sm12.kr) AS kr,
	(SELECT count(*) FROM sm12.kf) AS kf, to_regclass('sm12.kq2') AS kq2,
	(SELECT count(*) FROM sm12.kq) AS kq;
-- (main's sm12.kr, dropped and made again there, is another table: dropped
-- here first, the reset makes main's)
DROP TABLE sm12.kr;
SELECT dolt_reset('--hard', :'sm12_main');
SELECT format_type(atttypid, atttypmod) FROM pg_attribute
WHERE attrelid = 'sm12.p'::regclass AND attname = 'id';
SELECT * FROM sm12.c ORDER BY id;
SELECT * FROM sm12.d ORDER BY id;
SELECT format_type(typbasetype, typtypmod) FROM pg_type WHERE oid = 'sm12.k'::regtype;
SELECT * FROM sm12.kc ORDER BY id;
SELECT conrelid::regclass, pg_get_constraintdef(oid) FROM pg_constraint
WHERE confrelid = 'sm12.kp'::regclass ORDER BY conrelid::regclass::text;
SELECT to_regclass('sm12.kd') AS kd, (SELECT count(*) FROM sm12.kr) AS kr,
	(SELECT count(*) FROM sm12.kf) AS kf, to_regclass('sm12.kq2') AS kq2,
	(SELECT count(*) FROM sm12.kq) AS kq;
SELECT conname FROM pg_constraint WHERE conrelid = 'sm12.c'::regclass
  AND contype = 'f';
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'sm12_right');
DROP SCHEMA sm12 CASCADE;
SELECT length(dolt_commit('-Am', 'without sm12')::text) AS committed;

-- Emptying a table row-level security is forced on applies its rows as
-- the move writes them: refused, as writing them is, not left to a
-- conversion that can't be made.
CREATE ROLE regress_sm13 IN ROLE pg_prolly_write;
CREATE SCHEMA sm13 VERSIONED;
GRANT ALL ON SCHEMA sm13 TO regress_sm13;
SET ROLE regress_sm13;
CREATE TABLE sm13.t (pk int PRIMARY KEY, c text);
ALTER TABLE sm13.t ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
CREATE POLICY p ON sm13.t USING (true);
INSERT INTO sm13.t VALUES (1, 'one');
RESET ROLE;
SELECT length(dolt_commit('-Am', 'sm13 base')::text) AS committed;
SELECT dolt_branch('sm13_right');
SET ROLE regress_sm13;
UPDATE sm13.t SET c = '1';
ALTER TABLE sm13.t ALTER COLUMN c TYPE int USING c::int;
RESET ROLE;
SELECT (dolt_commit('-Am', 'sm13 main'))[1] AS sm13_main \gset
SELECT dolt_checkout('sm13_right');
SET ROLE regress_sm13;
SELECT dolt_reset('--hard', :'sm13_main');
RESET ROLE;
SELECT * FROM sm13.t;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'sm13_right');
DROP SCHEMA sm13 CASCADE;
DROP ROLE regress_sm13;
SELECT length(dolt_commit('-Am', 'without sm13')::text) AS committed;

-- Identity columns must keep their NOT NULL constraint names when a
-- move recreates the table or adds the column.  A child's inherited
-- constraint, or an unrelated table's constraint, can occupy the name
-- that automatic naming would otherwise choose.
CREATE SCHEMA sm14 VERSIONED;
CREATE TABLE sm14.q (a int NOT NULL, b int);
CREATE TABLE sm14.d (x int) INHERITS (sm14.q);
INSERT INTO sm14.q VALUES (10, 20);
INSERT INTO sm14.d VALUES (30, 40, 50);
CREATE TABLE sm14.u ("a b" int CONSTRAINT "custom nn" NOT NULL);
INSERT INTO sm14.u VALUES (60);
CREATE TABLE sm14.v (n text);
INSERT INTO sm14.v VALUES ('kept');
CREATE TABLE sm14.names (a int CONSTRAINT "u_a b_not_null" NOT NULL,
                        b int CONSTRAINT v_id_not_null NOT NULL);
CREATE TABLE sm14.ni (a int, b int);
ALTER TABLE sm14.ni ADD CONSTRAINT ni_nn NOT NULL a NO INHERIT;
CREATE TABLE sm14.nic (c int) INHERITS (sm14.ni);
INSERT INTO sm14.ni VALUES (1, 2);
INSERT INTO sm14.nic VALUES (NULL, 3, 4);
CREATE TABLE sm14.na (n text);
INSERT INTO sm14.na VALUES ('kept');
SELECT (dolt_commit('-Am', 'sm14 plain'))[1] AS sm14_plain \gset
ALTER TABLE sm14.q ALTER COLUMN a ADD GENERATED BY DEFAULT AS IDENTITY;
ALTER TABLE sm14.u ALTER COLUMN "a b" ADD GENERATED ALWAYS AS IDENTITY;
ALTER TABLE sm14.v ADD COLUMN id int CONSTRAINT "added nn" NOT NULL
  GENERATED BY DEFAULT AS IDENTITY;
ALTER TABLE sm14.ni ALTER COLUMN a ADD GENERATED ALWAYS AS IDENTITY;
ALTER TABLE sm14.na ADD COLUMN id int CONSTRAINT na_nn NOT NULL
  GENERATED BY DEFAULT AS IDENTITY;
ALTER TABLE sm14.na ALTER CONSTRAINT na_nn NO INHERIT;
SELECT (dolt_commit('-Am', 'sm14 identity'))[1] AS sm14_identity \gset
SELECT dolt_reset('--hard', :'sm14_plain');
SELECT test_fold_root() = prolly_root_hash(:'sm14_plain') AS same_root;
SELECT dolt_reset('--hard', :'sm14_identity');
SELECT test_fold_root() = prolly_root_hash(:'sm14_identity') AS same_root;
SELECT * FROM dolt_status();
SELECT conrelid::regclass, conname, conislocal, coninhcount
FROM pg_constraint WHERE connamespace = 'sm14'::regnamespace AND contype = 'n'
ORDER BY conrelid::regclass::text, conname;
SELECT c.relname, a.attname, a.attidentity FROM pg_attribute a
JOIN pg_class c ON c.oid = a.attrelid
WHERE c.relnamespace = 'sm14'::regnamespace AND c.relkind = 'r'
  AND a.attnum > 0 AND NOT a.attisdropped
ORDER BY c.relname, a.attnum;
SELECT * FROM sm14.q ORDER BY a;
SELECT * FROM sm14.d;
SELECT * FROM sm14.u;
SELECT * FROM sm14.v;
-- An identity's NOT NULL keeps NO INHERIT, made again or added: the child
-- still takes NULLs.
SELECT conrelid::regclass, conname, connoinherit FROM pg_constraint
WHERE conrelid IN ('sm14.ni'::regclass, 'sm14.nic'::regclass,
                   'sm14.na'::regclass) AND contype = 'n'
ORDER BY conrelid::regclass::text;
SELECT * FROM ONLY sm14.ni;
SELECT * FROM sm14.nic;
SELECT * FROM sm14.na;
INSERT INTO sm14.nic VALUES (NULL, 5, 6);
INSERT INTO sm14.ni OVERRIDING SYSTEM VALUE VALUES (NULL, 7);
INSERT INTO sm14.ni (b) VALUES (8) RETURNING *;
INSERT INTO sm14.na (n) VALUES ('new') RETURNING *;
INSERT INTO sm14.q (b) VALUES (70) RETURNING a, b;
INSERT INTO sm14.u DEFAULT VALUES RETURNING *;
INSERT INTO sm14.v (n) VALUES ('new') RETURNING *;
INSERT INTO sm14.q VALUES (NULL, 80);
INSERT INTO sm14.d VALUES (NULL, 80, 90);
INSERT INTO sm14.u OVERRIDING SYSTEM VALUE VALUES (NULL);
INSERT INTO sm14.v VALUES ('null', NULL);
SELECT dolt_reset('--hard', :'sm14_plain');
SELECT test_fold_root() = prolly_root_hash(:'sm14_plain') AS same_root;
SELECT * FROM dolt_status();
DROP SCHEMA sm14 CASCADE;
SELECT length(dolt_commit('-Am', 'without sm14')::text) AS committed;

DROP SCHEMA sm_full CASCADE;
DROP SCHEMA sm_empty;
DROP SCHEMA app CASCADE;

-- Two tables' names swapped: a hard reset back to the head, which matches
-- tables by name, would put each table's version into the other's
-- relation, and is refused; with the tables dropped it makes each its own
-- relation again.  A merge's abort does that itself.
CREATE SCHEMA sw VERSIONED;
CREATE TABLE sw.a (pk int PRIMARY KEY, x text);
CREATE TABLE sw.b (pk int PRIMARY KEY, y int);
INSERT INTO sw.a VALUES (1, 'a'); INSERT INTO sw.b VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'sw two')::text) AS committed;
SELECT 'sw.a'::regclass::oid AS sw_a, 'sw.b'::regclass::oid AS sw_b \gset
ALTER TABLE sw.a RENAME TO tmp;
ALTER TABLE sw.b RENAME TO a;
ALTER TABLE sw.tmp RENAME TO b;
SELECT dolt_reset('--hard');
DROP TABLE sw.a, sw.b;
SELECT dolt_reset('--hard');
SELECT 'sw.a'::regclass::oid = :sw_a AS a_again, 'sw.b'::regclass::oid = :sw_b AS b_again;
SELECT * FROM sw.a;
SELECT * FROM sw.b;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_branch('sw_side');
SELECT dolt_checkout('sw_side');
UPDATE sw.a SET x = 'side';
SELECT length(dolt_commit('-am', 'sw side')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE sw.a SET x = 'main';
SELECT length(dolt_commit('-am', 'sw main')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT dolt_merge('sw_side') IS NOT NULL AS merging;
ALTER TABLE sw.a RENAME TO tmp;
ALTER TABLE sw.b RENAME TO a;
ALTER TABLE sw.tmp RENAME TO b;
SELECT dolt_merge('--abort') IS NOT NULL AS aborted;
SELECT 'sw.a'::regclass::oid = :sw_a AS a_again, 'sw.b'::regclass::oid = :sw_b AS b_again;
SELECT * FROM sw.a;
SELECT * FROM sw.b;
SELECT count(*) AS changed FROM dolt_status;
RESET dolt.allow_commit_conflicts;
SELECT dolt_branch('-D', 'sw_side');
DROP SCHEMA sw CASCADE;
SELECT length(dolt_commit('-Am', 'sw gone')::text) AS committed;

-- Routines with SQL-standard bodies are parsed as they're made, so a move
-- makes each after what it reads: a materialized view, a table; and one a
-- table's default uses, before that table.
CREATE SCHEMA sb VERSIONED;
CREATE FUNCTION sb.seven() RETURNS int LANGUAGE SQL RETURN 7;
CREATE TABLE sb.t (id int PRIMARY KEY, d int DEFAULT sb.seven());
INSERT INTO sb.t (id) VALUES (1), (2);
CREATE MATERIALIZED VIEW sb.m AS SELECT count(*) AS n FROM sb.t;
CREATE FUNCTION sb.from_m() RETURNS bigint LANGUAGE SQL BEGIN ATOMIC SELECT n FROM sb.m; END;
CREATE FUNCTION sb.from_t() RETURNS bigint LANGUAGE SQL RETURN (SELECT sum(d) FROM sb.t);
SELECT length(dolt_commit('-Am', 'sb')::text) AS committed;
DROP FUNCTION sb.from_m();
DROP FUNCTION sb.from_t();
DROP MATERIALIZED VIEW sb.m;
DROP TABLE sb.t;
DROP FUNCTION sb.seven();
SELECT dolt_reset('--hard');
SELECT sb.from_m(), sb.from_t(), sb.seven();
INSERT INTO sb.t (id) VALUES (3);
SELECT * FROM sb.t ORDER BY id;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset('--hard');
DROP SCHEMA sb CASCADE;
SELECT length(dolt_commit('-Am', 'sb gone')::text) AS committed;

-- a cast made with one: after it
CREATE SCHEMA sc VERSIONED;
CREATE TYPE sc.wrap AS (v int);
CREATE FUNCTION sc.unwrap(sc.wrap) RETURNS int LANGUAGE SQL IMMUTABLE RETURN ($1).v;
CREATE CAST (sc.wrap AS int) WITH FUNCTION sc.unwrap(sc.wrap);
SELECT length(dolt_commit('-Am', 'sc')::text) AS committed;
SELECT dolt_hashof('HEAD') AS sc_head \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT to_regnamespace('sc') IS NULL AS sc_gone;
SELECT dolt_reset('--hard', :'sc_head');
SELECT ROW(4)::sc.wrap::int AS unwrapped;
SELECT count(*) AS changed FROM dolt_status;
DROP SCHEMA sc CASCADE;
SELECT length(dolt_commit('-Am', 'sc gone')::text) AS committed;
