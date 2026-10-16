-- A branch's sequences are its data: commits record their values, moves
-- set them, and a merge gives each the larger of the two.

CREATE SCHEMA app VERSIONED;
CREATE SEQUENCE app.s;
CREATE SEQUENCE app.untouched START 50;
CREATE TABLE app.t (id int PRIMARY KEY DEFAULT nextval('app.s'), v text);
INSERT INTO app.t (v) VALUES ('a'), ('b');
SELECT (dolt_commit('-Am', 'base'))[1] AS base \gset
SELECT nextval('app.s');
SELECT length(dolt_commit('-Am', 'a value taken')::text) AS committed;
SELECT dolt_reset('--hard', :'base');
SELECT nextval('app.s');		-- as the base left it
SELECT dolt_reset('--hard', :'base');

-- Merges: the larger value of each sequence the other side changed.
SELECT dolt_branch('feature');
INSERT INTO app.t (v) SELECT 'main' FROM generate_series(1, 3);
SELECT length(dolt_commit('-Am', 'main takes 3')::text) AS committed;
SELECT dolt_checkout('feature');
SELECT setval('app.s', 40);
INSERT INTO app.t (v) VALUES ('feature');
SELECT length(dolt_commit('-Am', 'feature goes to 41')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature'))[2:4];
SELECT last_value, is_called FROM app.s;
SELECT last_value, is_called FROM app.untouched;
INSERT INTO app.t (v) VALUES ('after');
SELECT id FROM app.t WHERE v = 'after';

-- and keeps ours when it's larger, or theirs didn't change it
SELECT dolt_checkout('feature');
SELECT nextval('app.untouched');
SELECT length(dolt_commit('-Am', 'feature takes untouched')::text) AS committed;
SELECT dolt_checkout('main');
SELECT setval('app.s', 1000);
SELECT length(dolt_commit('-Am', 'main jumps')::text) AS committed;
SELECT (dolt_merge('feature'))[2:4];
SELECT last_value, is_called FROM app.s;
SELECT last_value, is_called FROM app.untouched;

-- A sequence's value alone is a change to commit.
SELECT nextval('app.untouched');
SELECT length(dolt_commit('-Am', 'just a value')::text) AS committed;
SELECT dolt_commit('-Am', 'nothing');

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA app CASCADE;
