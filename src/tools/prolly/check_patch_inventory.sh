#!/bin/bash
#
# check_patch_inventory.sh
#	  Check that every core file the fork changes is in the patch inventory.
#
# The changed files are those that differ between HEAD and its merge base
# with upstream (postgres/master unless --upstream says otherwise), or the
# paths read from --changed, one per line.  Every changed file must either
# lie under a path the inventory marks "own" or be listed as a "patch", and
# every listed patch must still be a changed file, so the inventory never
# claims a change that is gone.
#
# Usage:
#	check_patch_inventory.sh [--inventory FILE] [--upstream REF | --base REV | --changed FILE]
#
# Exits 0 when the inventory matches, 1 when it does not (each problem is
# printed), and 2 on a usage or git error.
#
# src/tools/prolly/check_patch_inventory.sh

set -u

inventory="$(dirname "$0")/patch_inventory"
upstream=postgres/master
base=
changed=

usage()
{
	echo "usage: $0 [--inventory FILE] [--upstream REF | --base REV | --changed FILE]" >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
		--inventory) [ $# -ge 2 ] || usage; inventory=$2; shift 2 ;;
		--upstream) [ $# -ge 2 ] || usage; upstream=$2; shift 2 ;;
		--base) [ $# -ge 2 ] || usage; base=$2; shift 2 ;;
		--changed) [ $# -ge 2 ] || usage; changed=$2; shift 2 ;;
		*) usage ;;
	esac
done

if [ ! -r "$inventory" ]; then
	echo "cannot read patch inventory \"$inventory\"" >&2
	exit 2
fi

if ! inventory_text=$(cat "$inventory"); then
	exit 2
fi

declare -a own=()
declare -A patch=()
problems=0

problem()
{
	echo "$1"
	problems=$((problems + 1))
}

# is_own PATH: does PATH lie under (or at) one of the "own" paths?
is_own()
{
	local p
	for p in "${own[@]}"; do
		case "$p" in
			*/) [[ "$1" == "$p"* ]] && return 0 ;;
			*) [ "$1" = "$p" ] && return 0 ;;
		esac
	done
	return 1
}

# Read the inventory.  The "own" lines are read first, so that a patch line
# naming an own path is caught wherever it sits in the file.
lineno=0
while IFS= read -r line || [ -n "$line" ]; do
	lineno=$((lineno + 1))
	read -r kind path _ <<<"$line"
	[ -z "${kind:-}" ] && continue
	[[ "$kind" == \#* ]] && continue
	if [ "$kind" = own ]; then
		if [ -z "${path:-}" ] || [ -n "$(awk '{print $3}' <<<"$line")" ]; then
			problem "$inventory:$lineno: an own line takes exactly one path"
			continue
		fi
		own+=("$path")
	fi
done <<<"$inventory_text"

lineno=0
while IFS= read -r line || [ -n "$line" ]; do
	lineno=$((lineno + 1))
	read -r kind path layer reason <<<"$line"
	[ -z "${kind:-}" ] && continue
	[[ "$kind" == \#* ]] && continue
	case "$kind" in
		own) ;;
		patch)
			if [ -z "${reason:-}" ]; then
				problem "$inventory:$lineno: a patch line needs a path, a layer and a reason"
			elif ! [[ "$layer" =~ ^[0-9]+[a-z]?$ ]]; then
				problem "$inventory:$lineno: \"$layer\" is not a layer (a number, as 0 or 1a)"
			elif is_own "$path"; then
				problem "$inventory:$lineno: $path is the fork's own, not a core patch"
			elif [ -n "${patch[$path]:-}" ]; then
				problem "$inventory:$lineno: $path is listed twice"
			else
				patch[$path]=$lineno
			fi
			;;
		*)
			problem "$inventory:$lineno: unknown line kind \"$kind\""
			;;
	esac
done <<<"$inventory_text"

# The changed files.
if [ -n "$changed" ]; then
	if [ ! -r "$changed" ]; then
		echo "cannot read changed-file list \"$changed\"" >&2
		exit 2
	fi
	changed_files=$(cat "$changed") || exit 2
else
	if [ -z "$base" ]; then
		if ! base=$(git merge-base HEAD "$upstream"); then
			echo "cannot find the merge base of HEAD and $upstream (is it fetched?)" >&2
			exit 2
		fi
	fi
	# (renames are reported as a deletion and an addition, so both paths
	# are checked)
	if ! changed_files=$(git diff --name-only --no-renames "$base" HEAD); then
		exit 2
	fi
fi

declare -A seen=()
while IFS= read -r f; do
	[ -z "$f" ] && continue
	seen[$f]=1
	if is_own "$f"; then
		continue
	fi
	if [ -z "${patch[$f]:-}" ]; then
		problem "$f: changed, but not in the patch inventory"
	fi
done <<<"$changed_files"

while IFS= read -r f; do
	[ -z "$f" ] && continue
	if [ -z "${seen[$f]:-}" ]; then
		problem "$inventory:${patch[$f]}: $f is listed, but does not differ from upstream"
	fi
done < <(printf '%s\n' "${!patch[@]}" | sort)

if [ "$problems" -gt 0 ]; then
	echo "the patch inventory ($inventory) does not match the tree: $problems problem(s)"
	exit 1
fi
exit 0
