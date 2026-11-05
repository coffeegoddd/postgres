-- ours
ALTER TYPE rt.color ADD VALUE 'blue';
-- theirs
ALTER TYPE rt.color ADD VALUE 'pink';
-- expect
conflict object type color
