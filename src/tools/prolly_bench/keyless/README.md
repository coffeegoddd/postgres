# Tables without a primary key: heap, prolly and Dolt

Times the same work on a table without a primary key, and on the same table
with one, in stock Postgres (heap), page-based prolly and Dolt: loading a
million rows, single-row and 10,000-row writes, scans, and for prolly and
Dolt their version control (commit, status, diff, reset, branch, checkout,
fast-forward and three-way merges, cherry-pick, log). A second pair of
scripts merges six cases of keyless rows changed on two branches, to
compare how each treats them.

Needs Go, Python 3, the Postgres installs to compare (stock Postgres for
heap, this tree for prolly), and `dolt` and the `mysql` client on PATH.

```sh
W=/some/work/dir
./gen.py $W                                  # data.csv and the scripts
./run.sh $W heap heap /path/to/stock/bin
./run.sh $W prolly prolly /path/to/prolly/bin
./run.sh $W dolt dolt
./run.sh $W prolly prolly_keyed /path/to/prolly/bin   # and heap_keyed, dolt_keyed
```

Each run starts a fresh server with its own data directory under the work
directory, and writes `out-SCRIPT.tsv`: each step, the median of its
repetitions in ms (the mean of the middle two for an even count), then
every repetition. Run one at a time: they share
ports, and each should have the machine to itself.

The merge cases aren't timed: each prints the rows after each case's merge
and its conflicts, into `out-merge_pg.txt` or `out-merge_dolt.txt`:

```sh
./run.sh $W prolly merge_pg /path/to/prolly/bin
./run.sh $W dolt merge_dolt
```

## The runner

`main.go` runs a script on one connection and times its steps. A script is
SQL statements, each ending with `;` at the end of a line, in steps:

- `@step NAME [REPS]` starts a timed step, run REPS times (default 1);
- `@pre` starts SQL run untimed before each repetition, `@body` the timed
  SQL again;
- `@setup` starts SQL run once, untimed;
- `{i}` in SQL is replaced by the repetition's number, from 0.

`dml.tmpl` and `vc.tmpl` are the steps; `gen.py` fills in each engine's
dialect.
