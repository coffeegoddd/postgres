-- The base commit every merge scenario branches from.
CREATE SCHEMA rt;
CREATE TYPE rt.color AS ENUM ('red', 'green');
CREATE TABLE rt.acct (
	id int PRIMARY KEY,
	email text NOT NULL UNIQUE,
	name text,
	score int CHECK (score >= 0)
);
INSERT INTO rt.acct SELECT g, 'u' || g || '@x', 'n' || g, g FROM generate_series(1, 1000) g;
CREATE TABLE rt.item (id int PRIMARY KEY, acct int REFERENCES rt.acct, qty int, note text);
INSERT INTO rt.item SELECT g, 1 + g % 1000, g % 10, NULL FROM generate_series(1, 3000) g;
CREATE INDEX item_acct ON rt.item (acct);
CREATE TABLE rt.log (at int, msg text);
INSERT INTO rt.log SELECT g, 'm' || g % 5 FROM generate_series(1, 500) g;
CREATE TABLE rt.ticket (id serial PRIMARY KEY, note text);
