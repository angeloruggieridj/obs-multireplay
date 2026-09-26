#!/usr/bin/env bash
#
# Does our .deb load into a real OBS on a given Ubuntu? Run inside a clean
# ubuntu:<release> container (see the linux-load job in build-project.yaml):
#
#   linux-load-check.sh <our .deb> <obs>
#
# <obs> is an OBS release tag, whose own Ubuntu .deb is installed (it lives
# under /usr/local), or "ppa", which installs OBS from ppa:obsproject/obs-studio
# (it lives under /usr). Those are the two places OBS comes from on Ubuntu, and
# the plugin has to be found from both.
#
# Three checks, each answering a different question:
#   1. apt installs our package next to that OBS: the Depends are satisfiable;
#   2. ldd -r resolves every library and symbol the plugin imports;
#   3. OBS itself, started under Xvfb, logs that it LOADED the plugin — which
#      is the only proof that the folder we put it in is one that OBS searches.
set -euo pipefail

ours="$(realpath "$1")"
obs="$2"
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq curl ca-certificates xvfb xauth libgl1-mesa-dri binutils > /dev/null

if [ "$obs" = ppa ]; then
  apt-get install -y -qq software-properties-common > /dev/null
  add-apt-repository -y ppa:obsproject/obs-studio > /dev/null
  apt-get update -qq
  apt-get install -y -qq obs-studio > /dev/null
else
  . /etc/os-release
  deb="OBS-Studio-${obs}-Ubuntu-${VERSION_ID}-x86_64.deb"
  curl -fsSLO "https://github.com/obsproject/obs-studio/releases/download/${obs}/${deb}" \
    || { echo "::error::OBS ${obs} publishes no ${deb}"; exit 1; }
  apt-get install -y -qq "./${deb}" > /dev/null
fi
# OBS's own .deb installs under /usr/local without refreshing the linker
# cache; OBS finds its libraries through RPATH, ldd below does not.
ldconfig

echo "--- installing $(basename "$ours")"
apt-get install -y -qq "$ours" > /dev/null
echo "--- package contents"
dpkg -L obs-multireplay | grep -E "\.so$|/obs-multireplay$" | while read -r f; do
  if [ -L "$f" ]; then echo "$f -> $(readlink "$f")"; else echo "$f"; fi
done

status=0
so=/usr/lib/x86_64-linux-gnu/obs-plugins/obs-multireplay.so
out="$(ldd -r "$so" 2>&1)"
if printf "%s\n" "$out" | grep -E "not found|undefined symbol"; then
  echo "::error::unresolved imports in $so"
  readelf -d "$so" | grep NEEDED || true
  status=1
else
  echo "ok: ldd -r resolves every import of $so"
fi

# A config of our own: the first-run wizard off, and a MultiReplay config that
# does not open its setup dialog. Neither matters to the log lines we read, but
# a modal is noise in a log that is the whole verdict.
mkdir -p "$HOME/.config/obs-studio/plugin_config/obs-multireplay" "$HOME/mr-session"
printf '[General]\nFirstRun=true\n' > "$HOME/.config/obs-studio/user.ini"
printf '[General]\nEnableAutoUpdates=false\n' > "$HOME/.config/obs-studio/global.ini"
printf '{"sessionFolder":"%s","cameras":[{"sourceName":"LoadCheck","displayName":"C1"}]}\n' \
  "$HOME/mr-session" > "$HOME/.config/obs-studio/plugin_config/obs-multireplay/config.json"

export LIBGL_ALWAYS_SOFTWARE=1
# SIGINT is a clean OBS shutdown; KILL only if it will not go.
timeout -s INT -k 20 40 xvfb-run -a -s "-screen 0 1280x720x24" \
  obs --multi --disable-shutdown-check --disable-missing-files-check > /tmp/obs-stdout.txt 2>&1 || true

log="$(ls -t "$HOME"/.config/obs-studio/logs/*.txt 2>/dev/null | head -1 || true)"
if [ -z "$log" ]; then
  echo "::error::OBS wrote no log"; tail -40 /tmp/obs-stdout.txt; exit 1
fi
echo "--- OBS log: plugin lines"
grep -E "obs-multireplay|Skipping (legacy )?plugin|Failed to load|OBS [0-9]+\.[0-9]+" "$log" | head -40 || true

if grep -q "\[obs-multireplay\] plugin loaded successfully" "$log"; then
  echo "ok: OBS loaded the plugin"
else
  echo "::error::OBS did not load obs-multireplay"
  grep -iE "module|plugin" "$log" | head -60 || true
  status=1
fi
if grep -E "Failed to load module.*obs-multireplay|obs-multireplay.*(undefined symbol|cannot open shared)" "$log"; then
  echo "::error::OBS reported a failure loading obs-multireplay"
  status=1
fi
exit "$status"
