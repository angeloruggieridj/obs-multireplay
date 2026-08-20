#!/usr/bin/env bash
#
# obs-multireplay — every obs_module_text() key the code asks for must exist in
# every locale file.
#
# Why this is a script and not a habit: obs_module_text() returns THE KEY when
# it cannot find a translation, so a missing entry is not a blank label or a
# warning — it is the string "Dock.ZoneAngles" printed on a button, in every
# language at once. Three of them shipped that way, and the only way anybody
# would have noticed is by looking at that particular corner of the panel.
#
# The audit's own note on method: where an invariant matters, it needs a
# mechanical check in CI, not a sentence in a comment.

set -euo pipefail
cd "$(dirname "$0")/.."

keys="$(grep -ohE 'obs_module_text\("[^"]+"\)' src/*.cpp src/*.hpp \
	| sed -E 's/obs_module_text\("//; s/"\)//' | sort -u)"

if [ -z "${keys}" ]; then
	echo "check-locale-keys: found no keys at all — the grep is wrong" >&2
	exit 2
fi

fail=0
for ini in data/locale/*.ini; do
	have="$(grep -ohE '^[A-Za-z0-9._]+=' "${ini}" | sed 's/=$//' | sort -u)"
	missing="$(comm -23 <(echo "${keys}") <(echo "${have}") || true)"
	if [ -n "${missing}" ]; then
		echo "MISSING in ${ini}:" >&2
		echo "${missing}" | sed 's/^/  /' >&2
		fail=1
	fi
done

if [ "${fail}" -ne 0 ]; then
	echo "check-locale-keys: FAILED" >&2
	exit 1
fi

echo "check-locale-keys: $(echo "${keys}" | wc -l | tr -d ' ') key(s), all present in $(ls data/locale/*.ini | wc -l | tr -d ' ') locale file(s)"
