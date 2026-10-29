-- Schema changes: columns added (with a fast default) and dropped, a type
-- rewritten, an enum value inserted between others, indexes made and
-- dropped, a partition detached and one attached, a table renamed.
SET search_path = rt, public;
ALTER TABLE kinds ADD COLUMN extra int DEFAULT 42;
ALTER TABLE kinds DROP COLUMN mac8;
ALTER TABLE kinds ALTER COLUMN i2 TYPE int4;
ALTER TYPE mood ADD VALUE 'meh' BEFORE 'ok';
DROP INDEX kinds_ip_hash;
CREATE INDEX kinds_m_btree ON kinds (m);
CREATE INDEX kinds_extra_hash ON kinds USING hash (extra);
CREATE INDEX kinds_d_brin ON kinds USING brin (d) WITH (autosummarize = on);
ALTER TABLE acct ADD CONSTRAINT acct_score_even CHECK (score % 2 = 0) NOT VALID;
ALTER TABLE events DETACH PARTITION events_other;
CREATE TABLE events_2026 PARTITION OF events FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');
ALTER TABLE scratch RENAME TO scratch2;
ALTER TABLE node SET (fillfactor = 60);
CREATE INDEX node_path_spgist ON node USING spgist (path);
ALTER TABLE ticket RENAME COLUMN note TO memo;
CREATE TABLE ctas AS SELECT id, memo FROM ticket WHERE id % 2 = 0;
