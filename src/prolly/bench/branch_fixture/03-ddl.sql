-- The columns change: an expression replaced (a rewrite), a stored
-- generated column added and one made a plain column, identities altered.
SET search_path = rt, public;
ALTER TABLE item ALTER COLUMN k SET EXPRESSION AS (lower(b) || '/' || a);
ALTER TABLE item ADD COLUMN h int GENERATED ALWAYS AS (a * 3) STORED;
ALTER TABLE byd ALTER COLUMN sq DROP EXPRESSION;
ALTER TABLE byd ALTER COLUMN id SET GENERATED ALWAYS;
ALTER TABLE byd ALTER COLUMN id RESTART WITH 5000;
ALTER TABLE item ALTER COLUMN id SET INCREMENT BY 7;
INSERT INTO item (a, b) VALUES (77, 'after ddl');
INSERT INTO byd (a) VALUES (11);
