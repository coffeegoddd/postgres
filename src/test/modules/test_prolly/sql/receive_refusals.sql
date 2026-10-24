-- A value an extension type's binary receive refuses, which a table may
-- hold (its input took it), is kept as its text form (tag 1): only a data
-- exception (class 22), or an internal error the type's own code raised,
-- counts as a refusal.  Anything else receive raises (a serialization
-- failure, a statement too complex) fails the commit, so a value has one
-- form whenever it is folded.
CREATE DATABASE regress_receive_refusals;
\c regress_receive_refusals
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE SCHEMA rr VERSIONED;
-- (the type outside the versioned schemas, as an extension's would be)
CREATE SCHEMA rrx;
CREATE TYPE rrx.test_receiver;
CREATE FUNCTION rrx.test_receiver_in(cstring) RETURNS rrx.test_receiver
AS 'textin' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION rrx.test_receiver_out(rrx.test_receiver) RETURNS cstring
AS 'textout' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION rrx.test_receiver_send(rrx.test_receiver) RETURNS bytea
AS 'textsend' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION rrx.test_receiver_recv(internal) RETURNS rrx.test_receiver
AS '$libdir/test_prolly', 'test_receiver_recv' LANGUAGE C IMMUTABLE STRICT;
CREATE TYPE rrx.test_receiver (INPUT = rrx.test_receiver_in,
    OUTPUT = rrx.test_receiver_out, SEND = rrx.test_receiver_send,
    RECEIVE = rrx.test_receiver_recv, INTERNALLENGTH = variable,
    STORAGE = extended);

CREATE TABLE rr.t (id int PRIMARY KEY, v rrx.test_receiver);
INSERT INTO rr.t VALUES (1, 'taken'), (2, 'refuse'), (3, 'refuse internal');
-- each value's tag: 0 (its binary form) or 1 (its text form), the same
-- encoded alone as in a commit's batch
SELECT id, get_byte(prolly_canonical_value(v), 0) AS tag FROM rr.t ORDER BY id;
SELECT (dolt_commit('-Am', 'refused values'))[1] IS NOT NULL AS committed;
DROP TABLE rr.t;
SELECT (dolt_reset('--hard'))[1] AS reset;
SELECT id, v::text FROM rr.t ORDER BY id;

-- other errors of receive are raised, alone and in a commit
SELECT prolly_canonical_value('serialize'::rrx.test_receiver);
SELECT prolly_canonical_value('deep'::rrx.test_receiver);
INSERT INTO rr.t VALUES (4, 'serialize');
SELECT dolt_commit('-am', 'a serialization failure');
DELETE FROM rr.t WHERE id = 4;
INSERT INTO rr.t VALUES (5, 'deep');
SELECT dolt_commit('-am', 'too complex');
DELETE FROM rr.t WHERE id = 5;
SELECT (dolt_reset('--hard'))[1] AS clean;

\c contrib_regression
DROP DATABASE regress_receive_refusals;
