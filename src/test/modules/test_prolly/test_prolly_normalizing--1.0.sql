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
    ALIGNMENT = int4, STORAGE = extended);

-- Order the physical spellings as text, to exercise canonical key encoding.
CREATE FUNCTION test_normalizing_cmp(test_normalizing, test_normalizing) RETURNS int
AS 'bttextcmp' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_lt(test_normalizing, test_normalizing) RETURNS bool
AS 'text_lt' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_le(test_normalizing, test_normalizing) RETURNS bool
AS 'text_le' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_eq(test_normalizing, test_normalizing) RETURNS bool
AS 'texteq' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_ge(test_normalizing, test_normalizing) RETURNS bool
AS 'text_ge' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION test_normalizing_gt(test_normalizing, test_normalizing) RETURNS bool
AS 'text_gt' LANGUAGE internal IMMUTABLE STRICT;
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
