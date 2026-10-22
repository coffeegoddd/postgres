-- Reverse the ordering, as a real extension upgrade could do.
CREATE OR REPLACE FUNCTION test_ordering_cmp(test_ordering, test_ordering) RETURNS integer
LANGUAGE sql IMMUTABLE STRICT AS
' SELECT CASE WHEN ($1).v < ($2).v THEN 1 WHEN ($1).v > ($2).v THEN -1 ELSE 0 END ';
CREATE OR REPLACE FUNCTION test_ordering_lt(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v > ($2).v';
CREATE OR REPLACE FUNCTION test_ordering_le(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v >= ($2).v';
CREATE OR REPLACE FUNCTION test_ordering_ge(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v <= ($2).v';
CREATE OR REPLACE FUNCTION test_ordering_gt(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v < ($2).v';
