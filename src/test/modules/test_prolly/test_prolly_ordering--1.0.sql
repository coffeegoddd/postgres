-- An extension-owned comparator, including when used in a composite PK.
CREATE TYPE test_ordering AS (v integer);
CREATE FUNCTION test_ordering_cmp(test_ordering, test_ordering) RETURNS integer
LANGUAGE sql IMMUTABLE STRICT AS
' SELECT CASE WHEN ($1).v < ($2).v THEN -1 WHEN ($1).v > ($2).v THEN 1 ELSE 0 END ';
CREATE FUNCTION test_ordering_lt(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v < ($2).v';
CREATE OPERATOR < (LEFTARG = test_ordering, RIGHTARG = test_ordering,
  FUNCTION = test_ordering_lt);
CREATE FUNCTION test_ordering_le(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v <= ($2).v';
CREATE OPERATOR <= (LEFTARG = test_ordering, RIGHTARG = test_ordering,
  FUNCTION = test_ordering_le);
CREATE FUNCTION test_ordering_eq(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v = ($2).v';
CREATE OPERATOR = (LEFTARG = test_ordering, RIGHTARG = test_ordering,
  FUNCTION = test_ordering_eq);
CREATE FUNCTION test_ordering_ge(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v >= ($2).v';
CREATE OPERATOR >= (LEFTARG = test_ordering, RIGHTARG = test_ordering,
  FUNCTION = test_ordering_ge);
CREATE FUNCTION test_ordering_gt(test_ordering, test_ordering) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT ($1).v > ($2).v';
CREATE OPERATOR > (LEFTARG = test_ordering, RIGHTARG = test_ordering,
  FUNCTION = test_ordering_gt);
CREATE OPERATOR CLASS test_ordering_ops DEFAULT FOR TYPE test_ordering USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
  FUNCTION 1 test_ordering_cmp(test_ordering, test_ordering);
