#!/bin/bash
#
# test_upstream_sync.sh
#	  Tests of upstream_sync.sh: the catversion.h resolution on its own,
#	  then whole syncs between throwaway repositories.
#
# Usage:
#	test_upstream_sync.sh WORKDIR
#
# WORKDIR must not exist; the tests make their repositories in it and leave
# it behind for inspection, so the caller chooses a place it cleans up (CI
# uses the runner's temporary directory).
#
# src/tools/prolly/test_upstream_sync.sh

set -u

if [ $# -ne 1 ]; then
	echo "usage: $0 WORKDIR" >&2
	exit 2
fi
if [ -e "$1" ]; then
	echo "$1 already exists; give a directory that does not" >&2
	exit 2
fi

sync=$(cd "$(dirname "$0")" && pwd)/upstream_sync.sh
mkdir -p "$1" || exit 2
work=$(cd "$1" && pwd)
failures=0
tests=0

export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.com
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.com
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1

ok()
{
	tests=$((tests + 1))
	echo "ok $tests - $1"
}

not_ok()
{
	tests=$((tests + 1))
	failures=$((failures + 1))
	echo "not ok $tests - $1"
	[ -n "${2:-}" ] && echo "$2" | sed 's/^/# /'
}

# check NAME COMMAND...: the command succeeds.
check()
{
	local name=$1

	shift
	if "$@"; then
		ok "$name"
	else
		not_ok "$name"
	fi
}

catversion()
{
	printf '/* header */\n#define CATALOG_VERSION_NO\t%s\n/* footer */\n' "$1"
}

# --- the catversion.h resolution ---------------------------------------

out=$("$sync" --merge-catversion <(catversion 202601021) <(catversion 202601031))
check "upstream ahead: one past upstream's" [ "$out" = "$(catversion 202601032)" ]

out=$("$sync" --merge-catversion <(catversion 202601051) <(catversion 202601031))
check "fork ahead: one past the fork's" [ "$out" = "$(catversion 202601052)" ]

out=$("$sync" --merge-catversion <(catversion 202601031) <(catversion 202601031))
check "equal: one past both" [ "$out" = "$(catversion 202601032)" ]

"$sync" --merge-catversion <(echo nothing) <(catversion 202601031) >/dev/null 2>&1
check "a side without the define is an error" [ $? -ne 0 ]

# --- whole syncs ---------------------------------------------------------
#
# upstream.git plays postgres/postgres, fork.git the fork on GitHub, and
# clone the builder's checkout, with remotes "postgres" and "origin".

git init --quiet --bare -b master "$work/upstream.git"
git init --quiet --bare -b master "$work/fork.git"

git init --quiet -b master "$work/up"
mkdir -p "$work/up/src/include/catalog"
catversion 202601011 >"$work/up/src/include/catalog/catversion.h"
printf 'one\ntwo\nthree\n' >"$work/up/core.c"
git -C "$work/up" add -A
git -C "$work/up" commit --quiet -m "upstream start"
git -C "$work/up" push --quiet "$work/upstream.git" master
git -C "$work/up" push --quiet "$work/fork.git" master

git clone --quiet "$work/fork.git" "$work/clone"
git -C "$work/clone" remote add postgres "$work/upstream.git"
echo fork >"$work/clone/fork_only.c"
git -C "$work/clone" add fork_only.c
git -C "$work/clone" commit --quiet -m "fork change"
git -C "$work/clone" push --quiet origin master

upstream_commit()
{
	local msg=$1

	git -C "$work/up" add -A
	git -C "$work/up" commit --quiet -m "$msg"
	git -C "$work/up" push --quiet "$work/upstream.git" master
}

run_sync()
{
	(cd "$work/clone" && "$sync" --date "$1" >"$work/sync-$1.log" 2>&1)
}

# A stand-in for gh, for the syncs with --push: "pr list" prints the head
# branches in GH_STUB_OPEN_HEADS, and "pr create" logs its arguments and
# then succeeds, or fails as GH_STUB_CREATE says ("refuse": as GitHub does
# when the token may not open pull requests; "error": some other way).
mkdir -p "$work/bin"
cat >"$work/bin/gh" <<'STUB'
#!/bin/bash
echo "gh $*" >>"$GH_STUB_LOG"
case "$1 $2" in
	"pr list")
		[ -n "${GH_STUB_OPEN_HEADS:-}" ] && printf '%s\n' $GH_STUB_OPEN_HEADS
		exit 0 ;;
	"pr create")
		case "${GH_STUB_CREATE:-ok}" in
			ok) echo "https://github.com/coffeegoddd/postgres/pull/1"; exit 0 ;;
			refuse) echo "pull request create failed: GraphQL: GitHub Actions is not permitted to create or approve pull requests (createPullRequest)" >&2; exit 1 ;;
			*) echo "HTTP 502" >&2; exit 1 ;;
		esac ;;
esac
echo "unexpected gh call: $*" >&2
exit 1
STUB
chmod +x "$work/bin/gh"
export GH_STUB_LOG="$work/gh.log"

# run_push_sync DATE [VAR=VALUE...]: a sync with --push, gh stubbed.
run_push_sync()
{
	local date=$1

	shift
	: >"$GH_STUB_LOG"
	(cd "$work/clone" && env PATH="$work/bin:$PATH" "$@" \
		"$sync" --push --date "$date" >"$work/sync-$date.log" 2>&1)
}

# expect_status NAME WANT GOT LOG
expect_status()
{
	if [ "$3" -eq "$2" ]; then
		ok "$1"
	else
		not_ok "$1 (exit $3, expected $2)" "$(cat "$4")"
	fi
}

# nothing to merge
run_sync 2026-01-01
check "nothing to merge exits 3" [ $? -eq 3 ]
check "nothing to merge makes no branch" \
	[ -z "$(git -C "$work/clone" branch --list 'upstream-sync-*')" ]

# a plain merge
printf 'one\ntwo\nthree\nfour\n' >"$work/up/core.c"
upstream_commit "upstream adds a line"
upstream_head=$(git -C "$work/up" rev-parse HEAD)
run_sync 2026-01-02
status=$?
if [ $status -eq 0 ]; then
	ok "a clean merge exits 0"
else
	not_ok "a clean merge exits 0 (got $status)" "$(cat "$work/sync-2026-01-02.log")"
fi
check "the sync branch is checked out" \
	[ "$(git -C "$work/clone" symbolic-ref --short HEAD)" = upstream-sync-2026-01-02 ]
check "it is a merge commit of the fork's master and upstream's" \
	[ "$(git -C "$work/clone" rev-parse HEAD^1 HEAD^2)" = "$(git -C "$work/clone" rev-parse origin/master)
$upstream_head" ]
has_both_sides()
{
	[ -f "$work/clone/fork_only.c" ] && grep -q four "$work/clone/core.c"
}
check "the merge has both sides' changes" has_both_sides

# a second sync on the same day is refused
git -C "$work/clone" switch --quiet master
printf 'zero\none\ntwo\nthree\nfour\n' >"$work/up/core.c"
upstream_commit "upstream prepends a line"
run_sync 2026-01-02
check "a second sync for one day is skipped as under way (exit 4)" [ $? -eq 4 ]

# Land the first sync on the fork's master, as merging its pull request
# would, so later syncs start from it.
git -C "$work/clone" push --quiet origin upstream-sync-2026-01-02:master
git -C "$work/clone" fetch --quiet origin
git -C "$work/clone" reset --quiet --hard origin/master

# both sides bump catversion.h: resolved by bumping past both
catversion 202601021 >"$work/clone/src/include/catalog/catversion.h"
git -C "$work/clone" commit --quiet -am "fork bumps catversion"
git -C "$work/clone" push --quiet origin master
catversion 202601031 >"$work/up/src/include/catalog/catversion.h"
upstream_commit "upstream bumps catversion"
run_sync 2026-01-03
status=$?
if [ $status -eq 0 ]; then
	ok "a catversion.h conflict alone is resolved"
else
	not_ok "a catversion.h conflict alone is resolved (got $status)" "$(cat "$work/sync-2026-01-03.log")"
fi
check "catversion.h is one past both sides" \
	[ "$(cat "$work/clone/src/include/catalog/catversion.h")" = "$(catversion 202601032)" ]
resolution_committed()
{
	[ -z "$(git -C "$work/clone" status --porcelain)" ] &&
		[ "$(git -C "$work/clone" rev-parse HEAD^2)" = "$(git -C "$work/up" rev-parse HEAD)" ]
}
check "the resolution is committed as the merge" resolution_committed
check "the upstream change of that sync is merged too" grep -q zero "$work/clone/core.c"

# --- syncs that push and open the pull request ----------------------------

git -C "$work/clone" push --quiet origin upstream-sync-2026-01-03:master
git -C "$work/clone" fetch --quiet origin
git -C "$work/clone" switch --quiet master
git -C "$work/clone" reset --quiet --hard origin/master
echo new >"$work/up/up_new.c"
upstream_commit "upstream adds a file"

# an open sync pull request: skipped before anything is made
run_push_sync 2026-01-06 GH_STUB_OPEN_HEADS="feature/x upstream-sync-2025-12-29"
expect_status "an open sync pull request skips the sync (exit 4)" 4 $? "$work/sync-2026-01-06.log"
check "the skip makes no branch" \
	[ -z "$(git -C "$work/clone" branch --list upstream-sync-2026-01-06)" ]
check "the skip opens no pull request" bash -c "! grep -q 'pr create' '$GH_STUB_LOG'"

# GitHub refuses the token a pull request: the pushed branch is reported
run_push_sync 2026-01-06 GH_STUB_CREATE=refuse
expect_status "a refused pull request after a clean merge exits 5" 5 $? "$work/sync-2026-01-06.log"
check "the branch is pushed for opening by hand" \
	[ -n "$(git -C "$work/clone" ls-remote --heads origin upstream-sync-2026-01-06)" ]
check "the compare URL to open it is printed" \
	grep -q 'https://github.com/coffeegoddd/postgres/compare/master\.\.\.upstream-sync-2026-01-06?expand=1' \
	"$work/sync-2026-01-06.log"

# a re-run that day finds the branch: skipped, not a failure
git -C "$work/clone" switch --quiet master
git -C "$work/clone" branch --quiet -D upstream-sync-2026-01-06
run_push_sync 2026-01-06
expect_status "a re-run finding the branch on origin skips (exit 4)" 4 $? "$work/sync-2026-01-06.log"

# any other failure to open the pull request is an error
run_push_sync 2026-01-07 GH_STUB_CREATE=error
expect_status "another pull request failure exits 1" 1 $? "$work/sync-2026-01-07.log"

# the pull request is opened
git -C "$work/clone" switch --quiet master
run_push_sync 2026-01-08
expect_status "a pushed sync opens its pull request (exit 0)" 0 $? "$work/sync-2026-01-08.log"
check "the pull request is opened on the fork, from the sync branch to master" \
	grep -q -- '--repo coffeegoddd/postgres --base master --head upstream-sync-2026-01-08' "$GH_STUB_LOG"
check "the pushed branch is the merge" \
	[ "$(git -C "$work/clone" ls-remote --heads origin upstream-sync-2026-01-08 | cut -f1)" = \
	  "$(git -C "$work/clone" rev-parse upstream-sync-2026-01-08)" ]

# a conflict elsewhere: the merge is abandoned and reported
git -C "$work/clone" switch --quiet master
printf 'zero\nONE\ntwo\nthree\nfour\n' >"$work/clone/core.c"
git -C "$work/clone" commit --quiet -am "fork changes line one"
git -C "$work/clone" push --quiet origin master
printf 'zero\nuno\ntwo\nthree\nfour\n' >"$work/up/core.c"
upstream_commit "upstream changes line one"
run_sync 2026-01-04
check "a conflict it cannot resolve exits 2" [ $? -eq 2 ]
check "the conflicting file is reported" grep -q '^  core.c$' "$work/sync-2026-01-04.log"
left_clean()
{
	[ -z "$(git -C "$work/clone" status --porcelain --untracked-files=no)" ] &&
		[ "$(git -C "$work/clone" symbolic-ref --short HEAD)" = master ]
}
check "the checkout is left clean, back where it started" left_clean
check "the abandoned sync branch is deleted" \
	[ -z "$(git -C "$work/clone" branch --list upstream-sync-2026-01-04)" ]

# uncommitted changes are refused
echo dirty >>"$work/clone/fork_only.c"
run_sync 2026-01-05
check "a checkout with uncommitted changes is refused" [ $? -eq 1 ]

echo "1..$tests"
if [ "$failures" -gt 0 ]; then
	echo "# $failures of $tests tests failed; the repositories are in $work"
	exit 1
fi
exit 0
