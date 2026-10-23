-- A base type whose binary receive normalizes ASCII case.
CREATE TYPE test_normalizing;
CREATE FUNCTION test_normalizing_in(cstring) RETURNS test_normalizing
AS 'textin' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_out(test_normalizing) RETURNS cstring
AS 'textout' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_send(test_normalizing) RETURNS bytea
AS 'textsend' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_recv(internal) RETURNS test_normalizing
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE TYPE test_normalizing (INPUT = test_normalizing_in,
    OUTPUT = test_normalizing_out, SEND = test_normalizing_send,
    RECEIVE = test_normalizing_recv, INTERNALLENGTH = variable,
    ALIGNMENT = int4, STORAGE = extended, COLLATABLE = true);

-- The default ordering treats normalized spellings as equal.
CREATE FUNCTION test_normalizing_cmp(test_normalizing, test_normalizing) RETURNS int
LANGUAGE SQL IMMUTABLE STRICT
RETURN bttextcmp(lower($1::text), lower($2::text));
CREATE FUNCTION test_normalizing_lt(test_normalizing, test_normalizing) RETURNS bool
LANGUAGE SQL IMMUTABLE STRICT RETURN test_normalizing_cmp($1, $2) < 0;
CREATE FUNCTION test_normalizing_le(test_normalizing, test_normalizing) RETURNS bool
LANGUAGE SQL IMMUTABLE STRICT RETURN test_normalizing_cmp($1, $2) <= 0;
CREATE FUNCTION test_normalizing_eq(test_normalizing, test_normalizing) RETURNS bool
LANGUAGE SQL IMMUTABLE STRICT RETURN test_normalizing_cmp($1, $2) = 0;
CREATE FUNCTION test_normalizing_ge(test_normalizing, test_normalizing) RETURNS bool
LANGUAGE SQL IMMUTABLE STRICT RETURN test_normalizing_cmp($1, $2) >= 0;
CREATE FUNCTION test_normalizing_gt(test_normalizing, test_normalizing) RETURNS bool
LANGUAGE SQL IMMUTABLE STRICT RETURN test_normalizing_cmp($1, $2) > 0;
CREATE OPERATOR < (LEFTARG = test_normalizing, RIGHTARG = test_normalizing,
    PROCEDURE = test_normalizing_lt);
CREATE OPERATOR <= (LEFTARG = test_normalizing, RIGHTARG = test_normalizing,
    PROCEDURE = test_normalizing_le);
CREATE OPERATOR = (LEFTARG = test_normalizing, RIGHTARG = test_normalizing,
    PROCEDURE = test_normalizing_eq);
CREATE OPERATOR >= (LEFTARG = test_normalizing, RIGHTARG = test_normalizing,
    PROCEDURE = test_normalizing_ge);
CREATE OPERATOR > (LEFTARG = test_normalizing, RIGHTARG = test_normalizing,
    PROCEDURE = test_normalizing_gt);
CREATE OPERATOR CLASS test_normalizing_ops DEFAULT FOR TYPE test_normalizing USING btree AS
    OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
    FUNCTION 1 test_normalizing_cmp(test_normalizing, test_normalizing);

-- An unsafe ordering: receive changes its comparison value.
-- A base type whose binary receive normalizes ASCII case.
CREATE TYPE test_raw;
CREATE FUNCTION test_raw_in(cstring) RETURNS test_raw
AS 'textin' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_raw_out(test_raw) RETURNS cstring
AS 'textout' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_raw_send(test_raw) RETURNS bytea
AS 'textsend' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_raw_recv(internal) RETURNS test_raw
AS 'MODULE_PATHNAME', 'test_normalizing_recv' LANGUAGE C IMMUTABLE STRICT;
CREATE TYPE test_raw (INPUT = test_raw_in,
    OUTPUT = test_raw_out, SEND = test_raw_send,
    RECEIVE = test_raw_recv, INTERNALLENGTH = variable,
    ALIGNMENT = int4, STORAGE = extended, COLLATABLE = true);

CREATE FUNCTION test_raw_cmp(test_raw, test_raw) RETURNS int
AS 'bttextcmp' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_raw_lt(test_raw, test_raw) RETURNS bool
AS 'text_lt' LANGUAGE internal IMMUTABLE STRICT;
CREATE OPERATOR < (LEFTARG = test_raw, RIGHTARG = test_raw,
    PROCEDURE = test_raw_lt);
CREATE FUNCTION test_raw_le(test_raw, test_raw) RETURNS bool
AS 'text_le' LANGUAGE internal IMMUTABLE STRICT;
CREATE OPERATOR <= (LEFTARG = test_raw, RIGHTARG = test_raw,
    PROCEDURE = test_raw_le);
CREATE FUNCTION test_raw_eq(test_raw, test_raw) RETURNS bool
AS 'texteq' LANGUAGE internal IMMUTABLE STRICT;
CREATE OPERATOR = (LEFTARG = test_raw, RIGHTARG = test_raw,
    PROCEDURE = test_raw_eq);
CREATE FUNCTION test_raw_ge(test_raw, test_raw) RETURNS bool
AS 'text_ge' LANGUAGE internal IMMUTABLE STRICT;
CREATE OPERATOR >= (LEFTARG = test_raw, RIGHTARG = test_raw,
    PROCEDURE = test_raw_ge);
CREATE FUNCTION test_raw_gt(test_raw, test_raw) RETURNS bool
AS 'text_gt' LANGUAGE internal IMMUTABLE STRICT;
CREATE OPERATOR > (LEFTARG = test_raw, RIGHTARG = test_raw,
    PROCEDURE = test_raw_gt);
CREATE OPERATOR CLASS test_raw_ops DEFAULT FOR TYPE test_raw USING btree AS
    OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
    FUNCTION 1 test_raw_cmp(test_raw, test_raw);
