# Foreign keys take FOR KEY SHARE on the parent row: inserting children
# doesn't block, and isn't blocked by, an update of a non-key parent column;
# it does wait for an update of the key.

setup
{
	CREATE TABLE fk_parent (id int PRIMARY KEY, note text);
	CREATE TABLE fk_child (id int PRIMARY KEY, parent_id int REFERENCES fk_parent);
	INSERT INTO fk_parent VALUES (1, 'one');
}

teardown
{
	DROP TABLE fk_child, fk_parent;
}

session s1
step s1_begin		{ BEGIN; }
step s1_note		{ UPDATE fk_parent SET note = 'changed' WHERE id = 1; }
step s1_key			{ UPDATE fk_parent SET id = 2 WHERE id = 1; }
step s1_commit		{ COMMIT; }

session s2
step s2_child		{ INSERT INTO fk_child VALUES (10, 1); }
step s2_show		{ SELECT * FROM fk_child; }

permutation s1_begin s1_note s2_child s1_commit s2_show
permutation s1_begin s1_key s2_child s1_commit s2_show
