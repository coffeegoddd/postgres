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
DROP SCHEMA sm_full CASCADE;
DROP SCHEMA sm_empty;
DROP SCHEMA app CASCADE;
