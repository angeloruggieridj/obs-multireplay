#!/usr/bin/env bash
# obs-multireplay — §4.1: find comment lines that repeat VERBATIM in more
# than one place in src/.
#
# The comments in this codebase carry real content — the *why*, and often
# the bug that taught it — which is exactly what makes a stale duplicate
# dangerous: the code moves on, and a copy of the explanation left behind
# somewhere else goes on describing a decision that no longer holds. Found
# by hand once (buildChannelRow's own "the swap skips a column..." sentence,
# repeated word-for-word at its own call site); this is how the next one is
# found without re-reading 40,000 lines of comments looking for it.
#
# Advisory only: NOT wired into CI, for the same reason
# find-orphan-constants.sh is not — a short, common phrase can legitimately
# appear twice, and this cannot tell "stale copy" from "still true in both
# places" any more than that script can tell "orphan" from "read from a
# test". Read every hit before touching either copy.
#
# Usage: tools/find-duplicate-comments.sh [min-length]
#   min-length defaults to 50 characters: short enough to still repeat
#   legitimately (a one-line label, a section marker), so those are left
#   out rather than drowning the real hits in noise.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/src"
min_len="${1:-50}"

if [[ ! -d "$src" ]]; then
	echo "error: $src not found — run this from the repo root" >&2
	exit 1
fi

grep -rnE '^[[:space:]]*//' "$src" --include='*.cpp' --include='*.hpp' | awk -F: -v OFS=: -v minlen="$min_len" '
{
	file = $1; line = $2
	$1 = ""; $2 = ""
	text = $0
	sub(/^:+/, "", text)
	sub(/^[[:space:]]*\/\/[[:space:]]*/, "", text)
	gsub(/[[:space:]]+/, " ", text)
	sub(/^ /, "", text)
	sub(/ $/, "", text)
	if (length(text) < minlen) next
	# A divider line ("// ---...---") is a deliberate, ubiquitous section
	# marker in this codebase, not a duplicated explanation — skip
	# anything with no letters in it at all.
	if (text !~ /[A-Za-z]/) next
	locations[text] = locations[text] file ":" line "\n"
	count[text]++
}
END {
	found = 0
	for (t in count) {
		if (count[t] > 1) {
			found = 1
			print "----"
			print "\"" t "\""
			printf "%s", locations[t]
		}
	}
	if (!found)
		print "no duplicate comment lines found"
}
'
