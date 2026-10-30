#!/bin/bash
#
# upstream_sync.sh
#	  Merge postgres/postgres master into the fork's master on a sync branch.
#
# The fork carries its patches on top of postgres/postgres and takes
# upstream's changes by merging, never by rebasing, so that every fork
# commit keeps its identity and every merge shows exactly what upstream
# brought in.  This script:
#
#	1. fetches master from the "postgres" remote (adding the remote if the
#	   checkout lacks it) and from origin;
#	2. creates the branch upstream-sync-YYYY-MM-DD from origin/master;
#	3. merges postgres/master into it with a merge commit;
#	4. resolves the one conflict that recurs on its own: both sides
#	   bumping CATALOG_VERSION_NO in catversion.h, settled by bumping past
#	   both;
#	5. with --push, pushes the branch and opens its pull request, which
#	   must be merged as a merge commit.
#
# The remote is named "postgres", never "upstream": gh treats a remote
# named upstream as the repository to open pull requests against, and
# would open them on postgres/postgres.
#
# Usage:
#	upstream_sync.sh [--push] [--date YYYY-MM-DD]
#	upstream_sync.sh --merge-catversion OURS THEIRS
#
# The second form prints THEIRS with CATALOG_VERSION_NO set one past the
# larger of the two files' values; the sync uses it to resolve catversion.h.
#
# Exit status: 0 when the sync branch is made (and, with --push, pushed
# with its pull request open); 3 when master already contains
# postgres/master and there is nothing to merge; 2 when the merge has
# conflicts this script cannot resolve, in which case the merge is
# abandoned, the branch deleted and the conflicting files printed; 1 on
# any other error.
#
# src/tools/prolly/upstream_sync.sh

set -u

UPSTREAM_REMOTE=postgres
UPSTREAM_URL=https://github.com/postgres/postgres.git
FORK_REPO=${PROLLY_FORK_REPO:-coffeegoddd/postgres}
CATVERSION=src/include/catalog/catversion.h

die()
{
	echo "upstream_sync: $*" >&2
	exit 1
}

# catversion_of TEXT: the CATALOG_VERSION_NO in a catversion.h's text.
catversion_of()
{
	sed -n 's/^#define[[:space:]]\{1,\}CATALOG_VERSION_NO[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' <<<"$1"
}

# merge_catversion OURS THEIRS: THEIRS, with its version one past both.
# (each file is read once, so either may be a pipe)
merge_catversion()
{
	local ours_text theirs_text ours theirs next

	ours_text=$(cat "$1") || return 1
	theirs_text=$(cat "$2") || return 1
	ours=$(catversion_of "$ours_text")
	theirs=$(catversion_of "$theirs_text")
	if [ -z "$ours" ] || [ -z "$theirs" ]; then
		echo "upstream_sync: no CATALOG_VERSION_NO in one side of $CATVERSION" >&2
		return 1
	fi
	if [ "$ours" -gt "$theirs" ]; then
		next=$((ours + 1))
	else
		next=$((theirs + 1))
	fi
	sed "s/^\(#define[[:space:]]\{1,\}CATALOG_VERSION_NO[[:space:]]\{1,\}\)[0-9]\{1,\}/\1$next/" <<<"$theirs_text"
}

if [ "${1:-}" = --merge-catversion ]; then
	[ $# -eq 3 ] || die "usage: $0 --merge-catversion OURS THEIRS"
	merge_catversion "$2" "$3"
	exit $?
fi

push=false
date=$(date -u +%Y-%m-%d)
while [ $# -gt 0 ]; do
	case "$1" in
		--push) push=true; shift ;;
		--date) [ $# -ge 2 ] || die "--date needs a value"; date=$2; shift 2 ;;
		*) die "usage: $0 [--push] [--date YYYY-MM-DD]" ;;
	esac
done
[[ "$date" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || die "\"$date\" is not a date (YYYY-MM-DD)"
branch=upstream-sync-$date

cd "$(git rev-parse --show-toplevel)" || die "not in a git checkout"

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	die "the checkout has uncommitted changes; commit or stash them first"
fi
if git remote get-url upstream >/dev/null 2>&1; then
	echo "upstream_sync: warning: this checkout has a remote named \"upstream\", which makes gh target its repository; rename it (git remote rename upstream $UPSTREAM_REMOTE)" >&2
fi
if ! git remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
	git remote add "$UPSTREAM_REMOTE" "$UPSTREAM_URL" || die "cannot add the $UPSTREAM_REMOTE remote"
fi

git fetch --no-tags "$UPSTREAM_REMOTE" master || die "cannot fetch $UPSTREAM_REMOTE master"
git fetch --no-tags origin master || die "cannot fetch origin master"

upstream_rev=$(git rev-parse "$UPSTREAM_REMOTE/master") || exit 1
if git merge-base --is-ancestor "$upstream_rev" origin/master; then
	echo "origin/master already contains $UPSTREAM_REMOTE/master ($(git rev-parse --short "$upstream_rev")); nothing to merge"
	exit 3
fi

if git rev-parse --verify --quiet "refs/heads/$branch" >/dev/null; then
	die "branch $branch already exists"
fi
if [ -n "$(git ls-remote --heads origin "$branch")" ]; then
	die "branch $branch already exists on origin"
fi

start=$(git symbolic-ref --quiet --short HEAD || git rev-parse HEAD)
git switch --quiet --no-track -c "$branch" origin/master || die "cannot create $branch"

ncommits=$(git rev-list --count "origin/master..$upstream_rev")
message="Merge postgres/master ($(git rev-parse --short "$upstream_rev")) into the fork

Upstream sync $date: $ncommits commits from postgres/postgres master."

if ! git merge --no-ff --no-edit -m "$message" "$upstream_rev"; then
	conflicts=$(git diff --name-only --diff-filter=U)
	if [ -z "$conflicts" ]; then
		# (the merge stopped before merging anything, as when it would
		# overwrite an untracked file)
		git switch --quiet "$start" 2>/dev/null || git switch --quiet --detach "$start"
		git branch --quiet -D "$branch"
		die "the merge failed without conflicts; see git's message above"
	fi
	if [ "$conflicts" = "$CATVERSION" ]; then
		echo "resolving $CATVERSION: bumping CATALOG_VERSION_NO past both sides"
		# (the two sides are read from the index, so the merged file can
		# be written over the conflicted one directly; if that fails, the
		# merge is abandoned below, which restores it)
		if merge_catversion <(git show ":2:$CATVERSION") <(git show ":3:$CATVERSION") >"$CATVERSION" &&
			git add "$CATVERSION" &&
			git commit --quiet --no-edit; then
			conflicts=
		fi
	fi
	if [ -n "$conflicts" ]; then
		git merge --abort
		git switch --quiet "$start" 2>/dev/null || git switch --quiet --detach "$start"
		git branch --quiet -D "$branch"
		echo "the merge of $UPSTREAM_REMOTE/master has conflicts to resolve by hand:"
		sed 's/^/  /' <<<"$conflicts"
		exit 2
	fi
fi

echo "merged $UPSTREAM_REMOTE/master ($ncommits commits) into $branch"

# A core file upstream changed the same way the fork did no longer differs,
# and its inventory entry must go; report it here, as CI will.
inventory_check="src/tools/prolly/check_patch_inventory.sh"
if [ -x "$inventory_check" ]; then
	"$inventory_check" --upstream "$UPSTREAM_REMOTE/master" ||
		echo "upstream_sync: the patch inventory needs updating on $branch before it merges" >&2
fi

if $push; then
	git push --quiet origin "$branch" || die "cannot push $branch"
	gh pr create --repo "$FORK_REPO" --base master --head "$branch" \
		--title "Upstream sync $date" \
		--body "Merges postgres/postgres master ($(git rev-parse --short "$upstream_rev"), $ncommits commits) into the fork, made by \`src/tools/prolly/upstream_sync.sh\`.

**Merge this pull request with a merge commit.**  Squashing or rebasing it would rewrite upstream's commits and break the next sync's merge base." ||
		die "pushed $branch, but cannot open its pull request"
fi
exit 0
