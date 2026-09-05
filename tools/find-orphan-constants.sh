#!/usr/bin/env bash
# obs-multireplay — find k-prefixed constants declared in src/ with zero uses
# outside their own declaration line.
#
# The project's own convention names every constant kSomething
# (kMaxCameras, kNoInstant, kEventAngles...), which is what makes this
# mechanical: a declaration is one grep, and "does anything else in src/
# say this name" is another. Written after finding three of these by hand
# (kAngleKeyWidth, kAngleKeyMinWidth, kAngleLabelWidth — left behind when
# the per-camera angle matrix was replaced by clicking the picture) — the
# project's own comment on that fix put it plainly: "a number that is
# never read is a lie that ages." This is how the next one gets found
# without reading the whole tree by hand again.
#
# Advisory only: NOT wired into CI. A constant this flags might still be
# read from outside src/ (a test, a script) or be part of a public header
# some future caller is meant to use — read the hit before deleting
# anything.
#
# Usage: tools/find-orphan-constants.sh   (run from the repo root)

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/src"

if [[ ! -d "$src" ]]; then
	echo "error: $src not found — run this from the repo root" >&2
	exit 1
fi

# One declaration line per constant name, e.g.:
#   src/replay-core.hpp:48:constexpr int kDefaultPort = 8456;
declarations="$(grep -rnE '^\s*(static\s+)?(inline\s+)?constexpr\s+.*\bk[A-Z][A-Za-z0-9_]*\s*=' "$src" --include='*.hpp' --include='*.cpp')"

found=0
while IFS= read -r line; do
	[[ -z "$line" ]] && continue
	file="${line%%:*}"
	rest="${line#*:}"
	lineno="${rest%%:*}"
	name="$(sed -nE 's/.*\b(k[A-Z][A-Za-z0-9_]*)\s*=.*/\1/p' <<<"$line")"
	[[ -z "$name" ]] && continue

	# Every match for the name in src/, minus the declaration line itself.
	# grep exits 1 on "found nothing", which for an orphan is the whole
	# point of asking — not an error this script should abort on.
	uses="$(grep -rnw "$name" "$src" --include='*.hpp' --include='*.cpp' | grep -vF "$file:$lineno:" || true)"
	if [[ -z "$uses" ]]; then
		found=$((found + 1))
		echo "$file:$lineno: $name — no use found outside its own declaration"
	fi
done <<<"$declarations"

if [[ $found -eq 0 ]]; then
	echo "no orphan constants found"
fi
exit 0
