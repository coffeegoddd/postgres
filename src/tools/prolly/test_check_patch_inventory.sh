#!/bin/bash
#
# test_check_patch_inventory.sh
#	  Tests of check_patch_inventory.sh: it passes on a matching inventory
#	  and fails on each kind of mismatch.
#
# The inventories and changed-file lists are given to the check through
# process substitution, so the tests need no repository and leave no files.
#
# src/tools/prolly/test_check_patch_inventory.sh

set -u

check="$(dirname "$0")/check_patch_inventory.sh"
failures=0
tests=0

# expect STATUS PATTERN NAME INVENTORY CHANGED: the check exits with STATUS
# and, if PATTERN is not empty, prints a line matching it.
expect()
{
	local want=$1 pattern=$2 name=$3 inv=$4 changed=$5 out status

	tests=$((tests + 1))
	out=$("$check" --inventory <(printf '%s' "$inv") --changed <(printf '%s' "$changed") 2>&1)
	status=$?
	if [ "$status" -ne "$want" ]; then
		echo "not ok $tests - $name: exit status $status, expected $want"
		echo "$out" | sed 's/^/# /'
		failures=$((failures + 1))
	elif [ -n "$pattern" ] && ! grep -q -- "$pattern" <<<"$out"; then
		echo "not ok $tests - $name: no line matching \"$pattern\""
		echo "$out" | sed 's/^/# /'
		failures=$((failures + 1))
	else
		echo "ok $tests - $name"
	fi
}

inventory='# a comment
own   src/backend/prolly/
own   .github/workflows/prolly.yml

patch src/backend/Makefile 0 builds the engine
patch src/include/Makefile 1a installs the headers
'

expect 0 '' "listed patches and own files pass" "$inventory" \
'src/backend/Makefile
src/include/Makefile
src/backend/prolly/registry.c
.github/workflows/prolly.yml
'

expect 1 'src/backend/tcop/postgres.c: changed, but not in the patch inventory' \
	"an unlisted core change fails" "$inventory" \
'src/backend/Makefile
src/include/Makefile
src/backend/tcop/postgres.c
'

expect 0 '' "the same change passes once it is listed" \
"$inventory
patch src/backend/tcop/postgres.c 3 starts the engine at backend start
" \
'src/backend/Makefile
src/include/Makefile
src/backend/tcop/postgres.c
'

expect 1 'src/include/Makefile is listed, but does not differ from upstream' \
	"a listed file that no longer differs fails" "$inventory" \
'src/backend/Makefile
'

expect 1 'src/backend/prolly-extra.c: changed, but not in the patch inventory' \
	"an own directory does not cover a sibling sharing its prefix" "$inventory" \
'src/backend/Makefile
src/include/Makefile
src/backend/prolly-extra.c
'

expect 1 '.github/workflows/prolly.yml.orig: changed, but not in the patch inventory' \
	"an own file does not cover a longer name" "$inventory" \
'src/backend/Makefile
src/include/Makefile
.github/workflows/prolly.yml.orig
'

expect 1 'is the fork.s own, not a core patch' "listing an own file as a patch fails" \
"$inventory
patch src/backend/prolly/registry.c 0 not a patch
" \
'src/backend/Makefile
src/include/Makefile
src/backend/prolly/registry.c
'

expect 1 'needs a path, a layer and a reason' "a patch line without a reason fails" \
"$inventory
patch src/backend/tcop/postgres.c 3
" \
'src/backend/Makefile
src/include/Makefile
src/backend/tcop/postgres.c
'

expect 1 'is not a layer' "a patch line with a bad layer fails" \
"$inventory
patch src/backend/tcop/postgres.c three starts the engine
" \
'src/backend/Makefile
src/include/Makefile
src/backend/tcop/postgres.c
'

expect 1 'is listed twice' "a file listed twice fails" \
"$inventory
patch src/backend/Makefile 0 again
" \
'src/backend/Makefile
src/include/Makefile
'

expect 1 'unknown line kind' "an unknown line kind fails" \
"$inventory
patched src/backend/tcop/postgres.c 3 a typo
" \
'src/backend/Makefile
src/include/Makefile
'

expect 1 'an own line takes exactly one path' "an own line with extra words fails" \
"$inventory
own src/tools/prolly/ and more
" \
'src/backend/Makefile
src/include/Makefile
'

echo "1..$tests"
if [ "$failures" -gt 0 ]; then
	echo "# $failures of $tests tests failed"
	exit 1
fi
exit 0
