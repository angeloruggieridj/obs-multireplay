#!/usr/bin/env python3
"""Decide, record and publish which OBS Studio versions this plugin supports.

Ported from obs-playlist-deck, where the README used to claim a range by hand
while CI tested a hard-coded pair of versions, and the two drifted apart. Here
the range is derived from what CI actually compiled: one probe per OBS minor,
a contiguity rule, and a manifest the README (both languages) is generated from.

An SDK that fails to build on our runner is recorded as unverifiable, never as
incompatible; conflating the two pins the declared floor above the plugin's
real one.

What differs from obs-playlist-deck, and why:
- the floor is the minimum the README already declared (OBS 32), not the
  oldest API the code calls;
- the README is bilingual, so there are two generated blocks, one per language;
- the SDK the released binaries are built against is pinned in buildspec.json,
  together with per-platform hashes and the obs-deps/Qt releases that must
  follow it — so --write does not move it. It is rendered in the README and
  --check keeps that row honest, but bumping it stays a deliberate, manual step.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The oldest OBS the plugin has ever been declared for and run end to end (the
# packet tap on Branch Output's encoders, the dock, the replay source). Probing
# lower would only tell us whether older headers compile, which is not a claim
# anybody has asked this plugin to make.
FLOOR = (32, 0)
FLOOR_REASON = "OBS 32 is the oldest release obs-multireplay is declared for and run against"

_TAG = re.compile(r"^([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([0-9A-Za-z.]+))?$")


def parse_version(tag: str) -> tuple[int, int, int, str] | None:
    """Split a tag into (major, minor, patch, suffix); suffix is "" when stable."""
    match = _TAG.match(tag)
    if not match:
        return None
    major, minor, patch, suffix = match.groups()
    return int(major), int(minor), int(patch), suffix or ""


def is_stable(version: tuple[int, int, int, str]) -> bool:
    return version[3] == ""


def sort_key(version: tuple[int, int, int, str]) -> tuple:
    # A stable release outranks every prerelease of the same X.Y.Z. Among
    # prereleases, numeric runs within the suffix are compared as integers
    # (so beta9 < beta10 < rc1), while alphabetic runs compare as text
    # (rc > beta). This ensures double-digit prerelease numbers sort correctly.
    major, minor, patch, suffix = version
    if suffix == "":
        # Stable: rank above all prereleases
        return (major, minor, patch, 1)
    # Prerelease: split into alternating alpha and numeric runs, converting
    # numeric runs to ints for numerical comparison. Each run is wrapped in a
    # 2-tuple to ensure type safety: (0, int_value) for digits, (1, str_value)
    # for text. This prevents TypeError from comparing int to str directly.
    parts = re.findall(r"\d+|\D+", suffix)
    normalized = tuple(
        (0, int(part)) if part.isdigit() else (1, part)
        for part in parts
    )
    return (major, minor, patch, 0, normalized)


def _parsed(tags: list[str]) -> list[tuple[int, int, int, str]]:
    return [v for v in (parse_version(tag) for tag in tags) if v is not None]


def _format(version: tuple[int, int, int, str]) -> str:
    major, minor, patch, suffix = version
    base = f"{major}.{minor}.{patch}"
    return f"{base}-{suffix}" if suffix else base


def select_grid(tags: list[str]) -> list[str]:
    """One candidate per minor at or above the floor: its first patch, X.Y.0.

    The README claims "X.Y+", and the version that makes that sentence true is
    X.Y.0 — probing a later patch would verify a different claim. A minor whose
    .0 was never tagged is skipped rather than approximated.
    """
    minors = {
        (v[0], v[1])
        for v in _parsed(tags)
        if is_stable(v) and v[2] == 0 and (v[0], v[1]) >= FLOOR
    }
    return [f"{major}.{minor}.0" for major, minor in sorted(minors)]


def highest_stable(tags: list[str]) -> str:
    stable = [v for v in _parsed(tags) if is_stable(v)]
    if not stable:
        # Not SystemExit: that class is not an Exception, so it would escape
        # main()'s catch-all and exit 1 -- the one code reserved for "the
        # plugin does not compile against a probed OBS version". An empty
        # stable-tag list says nothing about the plugin at all.
        raise RangeError("the tag list contains no stable OBS release")
    return _format(max(stable, key=sort_key))


def qualifying_beta(tags: list[str]) -> str | None:
    """The newest prerelease, but only when it is ahead of the newest stable.

    A prerelease of a line that has already shipped is not forward-looking
    information, it is history — which is exactly what the old hard-coded
    matrix was still testing.
    """
    pres = [v for v in _parsed(tags) if not is_stable(v)]
    if not pres:
        return None
    newest = max(pres, key=sort_key)
    stable = parse_version(highest_stable(tags))
    return _format(newest) if sort_key(newest) > sort_key(stable) else None


class RangeError(Exception):
    """No supported range can be derived from these results."""


def _minor_label(candidate: str) -> str:
    major, minor, _patch, _suffix = parse_version(candidate)
    return f"{major}.{minor}"


def derive_range(results: dict[str, dict], grid: list[str], max_tested: str) -> dict:
    """The declared minimum is the start of the green block reaching the top.

    Not the lowest minor that compiles: if 30.0 passes and 30.1 does not,
    "30.0+" is a lie to everyone running 30.1. So the block is walked down from
    the newest minor and stops at the first candidate that is not green,
    whatever the reason.

    Reaching the top of the grid is necessary but not sufficient: the range
    this function returns is only as defensible as max_tested itself, which
    must independently be green -- see the check below.
    """
    if not grid:
        raise RangeError("the version grid is empty")

    newest = grid[-1]
    if results.get(newest, {}).get("status") != "ok":
        raise RangeError(
            f"the newest probed minor ({newest}) is not green, so no range "
            f"can be declared against {max_tested}"
        )

    # grid[-1] is the newest minor's first patch (X.Y.0), not necessarily
    # max_tested: build_matrix probes max_tested as a separate candidate
    # whenever a later patch of that minor has shipped (32.2.0 vs 32.2.2),
    # and it is max_tested -- not grid[-1] -- that is marked `required`, that
    # the README calls "Built against", and that this function is about to
    # name as the top of the declared range. The grid reaching its top green
    # says nothing about that specific tag; only checking it directly does.
    # Without this, a required probe could fail here without ever tripping
    # this function, and the range would still get declared against it.
    #
    # Phase matters here in a way it does not for the check above: a
    # plugin-build failure at max_tested is not "no range can be derived" --
    # it is a known, specific incompatibility, and _report's own broken-list
    # check exists precisely to report that as EXIT_INCOMPATIBLE with a
    # message naming the plugin as the cause. Raising RangeError for it here
    # would reach the caller first and relabel a "the plugin is broken"
    # finding as "the range is undecidable", which sends whoever reads the
    # exit code hunting in the wrong place. Only a non-plugin reason (the
    # SDK itself would not build, or the probe never reported at all) earns
    # the RangeError treatment; a plugin-build failure falls through and
    # lets the grid walk below proceed, so build_manifest still succeeds and
    # _report's later broken check is the one that reports it.
    max_tested_result = results.get(max_tested) or {}
    max_tested_green = max_tested_result.get("status") == "ok"
    max_tested_is_plugin_incompatibility = max_tested_result.get("phase") == "plugin-build"
    if not max_tested_green and not max_tested_is_plugin_incompatibility:
        raise RangeError(
            f"max_tested ({max_tested}) could not be built on the runner, "
            f"so the range cannot be declared without it"
        )

    index = len(grid) - 1
    while index > 0 and results.get(grid[index - 1], {}).get("status") == "ok":
        index -= 1

    gaps, unverifiable = [], []
    for candidate in grid[:index]:
        result = results.get(candidate)
        # A probe we never got back says as little as one whose SDK would not
        # build: both are unknown, and unknown is not the same as unsupported.
        if result is None:
            unverifiable.append(_minor_label(candidate))
        elif result.get("status") == "fail":
            # A failure: was it the plugin or the SDK?
            if result.get("phase") == "obs-build":
                unverifiable.append(_minor_label(candidate))
            else:
                gaps.append(_minor_label(candidate))
        # Else: status is ok, don't report success below the minimum

    return {
        "min_supported": _minor_label(grid[index]),
        "gaps": gaps,
        "unverifiable": unverifiable,
    }


MANIFEST_PATH = ROOT / "obs-compat.json"


def load_manifest(path: Path = MANIFEST_PATH) -> dict | None:
    """The manifest, or None on the first ever run — see build_matrix's bootstrap."""
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def save_manifest(data: dict, path: Path = MANIFEST_PATH) -> None:
    # newline="\n": this file is committed and diffed, and Python's text mode
    # would write CRLF on Windows, where this repository is mostly edited.
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8", newline="\n")


def build_manifest(results: dict[str, dict], grid: list[str], max_tested: str,
                   beta: str | None, generated: str) -> dict:
    derived = derive_range(results, grid, max_tested)
    return {
        "generated": generated,
        "floor": {"version": f"{FLOOR[0]}.{FLOOR[1]}", "reason": FLOOR_REASON},
        "min_supported": derived["min_supported"],
        "max_tested": max_tested,
        "beta_tested": beta,
        "gaps": derived["gaps"],
        "unverifiable": derived["unverifiable"],
        "results": dict(sorted(results.items())),
    }


README_START = "<!-- obs-compat:start -->"
README_END = "<!-- obs-compat:end -->"
BUILDSPEC_REL = Path("buildspec.json")

# The README is bilingual — an English half and an Italian half, each with its
# own Requirements table — so the generator owns one block per language. Both
# are rendered from the same manifest, so they cannot disagree with each other
# any more than either can disagree with obs-compat.json.
LANGS = ("en", "it")
MARKERS = {
    "en": (README_START, README_END),
    "it": ("<!-- obs-compat-it:start -->", "<!-- obs-compat-it:end -->"),
}

# Every label the generated blocks print, per language. The version numbers,
# tags and emoji are the same in both; only the words change.
TEXT = {
    "en": {
        "obs": "OBS Studio",
        "verified": "Verified by",
        "verified_value": "Compile and link against each version's OBS SDK in CI — not a runtime test.",
        "built": "Built against",
        "built_value": "{version} (the OBS SDK the released binaries are compiled with)",
        "beta": "Also builds against",
        "beta_value": "{beta} (prerelease, not supported)",
        "unverifiable": "Not verifiable in CI",
        "summary": "Every OBS version CI probed",
        "head": "| OBS | Result | Built on |",
        "ok": "✅ compiles and links",
        "incompatible": "❌ does not compile",
        "unbuildable": "⚠️ SDK could not be built in CI",
        "generated": "Generated from [`obs-compat.json`](obs-compat.json) by `tools/obs_compat.py`.",
    },
    "it": {
        "obs": "OBS Studio",
        "verified": "Verificato da",
        "verified_value": "Compilazione e link contro l'SDK di ogni versione di OBS in CI — non è un test a runtime.",
        "built": "Compilato contro",
        "built_value": "{version} (l'SDK di OBS con cui sono compilate le build pubblicate)",
        "beta": "Compila anche contro",
        "beta_value": "{beta} (pre-release, non supportata)",
        "unverifiable": "Non verificabili in CI",
        "summary": "Tutte le versioni di OBS provate dalla CI",
        "head": "| OBS | Esito | Compilato su |",
        "ok": "✅ compila e linka",
        "incompatible": "❌ non compila",
        "unbuildable": "⚠️ l'SDK non si è potuto compilare in CI",
        "generated": "Generato da [`obs-compat.json`](obs-compat.json) con `tools/obs_compat.py`.",
    },
}

# Static rows. A Markdown table cannot be split by an HTML comment and still
# render, so the generator owns the whole Requirements table; edit these here.
STATIC_ROWS = {
    "en": [
        ("Branch Output",
         "[Download](https://github.com/OPENSPHERE-Inc/branch-output/releases) · "
         "[repository](https://github.com/OPENSPHERE-Inc/branch-output) — the recording "
         "layer; MultiReplay does nothing without it"),
        ("Platforms",
         "Windows (primary) · macOS · Linux: Ubuntu 24.04 and 26.04 (X11/XWayland; under "
         "native Wayland the embedded previews say so in the box rather than going black)"),
    ],
    "it": [
        ("Branch Output",
         "[Download](https://github.com/OPENSPHERE-Inc/branch-output/releases) · "
         "[repository](https://github.com/OPENSPHERE-Inc/branch-output) — è lo strato che "
         "registra, e senza di lui MultiReplay non fa niente"),
        ("Piattaforme",
         "Windows (principale) · macOS · Linux: Ubuntu 24.04 e 26.04 (X11/XWayland; sotto "
         "Wayland nativo le anteprime lo scrivono nel riquadro invece di restare nere)"),
    ],
}


# How each probe environment reads to someone who has never opened the
# workflow. "jammy" and "native" are our words, not theirs.
ENV_LABELS = {
    "native": "Ubuntu 24.04",
    "jammy": "Ubuntu 22.04 (container)",
}


def _probe_outcome(result: dict, lang: str = "en") -> str:
    """What one probe proved, phrased for a reader rather than for the tool.

    The phase carries the weight here exactly as it does everywhere else: a
    plugin-build failure means this plugin does not compile against that OBS,
    while an obs-build failure only means we could not produce that SDK on our
    runner and says nothing about the plugin. The two must not collapse into a
    single red mark in the one table a non-contributor actually reads.
    """
    text = TEXT[lang]
    if result.get("status") == "ok":
        return text["ok"]
    if result.get("phase") == "plugin-build":
        return text["incompatible"]
    return text["unbuildable"]


def render_probe_table(manifest: dict, lang: str = "en") -> str:
    """Every version CI probed, so the declared range can be checked, not trusted.

    The range above it is a conclusion. Without this a reader has to open
    obs-compat.json to learn which versions were actually tried, in which
    environment, and what each one proved.
    """
    results = manifest.get("results", {})
    # Sorted as versions, not as text: "32.2.10" precedes "32.2.2"
    # lexicographically, and a reader scanning for the newest row would be
    # handed the wrong one. Unparseable keys sort first rather than raising.
    ordered = sorted(results, key=lambda v: sort_key(parse_version(v) or (0, 0, 0, "")))
    lines = [TEXT[lang]["head"], "|---|---|---|"]
    for version in ordered:
        result = results[version]
        env = result.get("env", "")
        lines.append(
            f"| `{version}` | {_probe_outcome(result, lang)} | {ENV_LABELS.get(env, env or '—')} |"
        )
    return "\n".join(lines)


def render_readme_section(manifest: dict, lang: str = "en",
                          built_against: str | None = None) -> str:
    """One language's Requirements table plus the folded evidence under it.

    built_against is buildspec.json's OBS version (see the module docstring for
    why it is not max_tested here). write() and check() always pass it; callers
    that only care about the range may leave it out and get no such row.
    """
    text = TEXT[lang]
    rows = [
        (text["obs"], f"**{manifest['min_supported']} – {manifest['max_tested']}**"),
        (text["verified"], text["verified_value"]),
    ]
    if built_against:
        rows.append((text["built"], text["built_value"].format(version=built_against)))
    beta = manifest.get("beta_tested")
    # beta_tested records what was *measured* -- red or green, see
    # build_manifest -- because needs_full_run needs to know a beta was
    # probed at all, not just that it passed. But advertising "also builds
    # against" here is a claim, and the only evidence that can back it is the
    # beta's own probe result. Rendering the row off beta_tested's mere
    # presence would advertise a beta that failed at plugin-build as something
    # this plugin builds against, contradicted by this same manifest's
    # `results` two keys away.
    if beta and manifest.get("results", {}).get(beta, {}).get("status") == "ok":
        rows.append((text["beta"], text["beta_value"].format(beta=beta)))
    if manifest.get("unverifiable"):
        rows.append((text["unverifiable"], ", ".join(manifest["unverifiable"])))
    rows.extend(STATIC_ROWS[lang])
    lines = ["| | |", "|---|---|"]
    lines += [f"| **{label}** | {value} |" for label, value in rows]

    # Conclusion first, evidence behind a fold: the range is what most readers
    # came for, but a claim nobody can check is how a hand-written range goes
    # stale. The table is generated from the same manifest as the range, so the
    # two cannot drift.
    return (
        "\n".join(lines)
        + f"\n\n<details>\n<summary>{text['summary']}</summary>\n\n"
        + render_probe_table(manifest, lang)
        # Deliberately no generation date here. This block is guarded by an
        # equality check, so anything that changes when the facts have not --
        # a timestamp above all -- makes every matrix run demand a cosmetic
        # regeneration and turns the weekly run into a failure nobody reads.
        # The date lives in obs-compat.json, which this points at.
        + f"\n\n{text['generated']}\n</details>"
    )


def _find_markers(text: str, lang: str = "en") -> tuple[int, int]:
    start_marker, end_marker = MARKERS[lang]
    start, end = text.find(start_marker), text.find(end_marker)
    if start == -1 or end == -1 or end < start:
        raise RangeError(f"README is missing the {start_marker} / {end_marker} markers")
    return start, end


def replace_between_markers(text: str, body: str, lang: str = "en") -> str:
    start, end = _find_markers(text, lang)
    return text[:start] + MARKERS[lang][0] + "\n" + body + "\n" + text[end:]


def _marker_region(text: str, lang: str = "en") -> str:
    """The text strictly between one language's markers, markers excluded.

    check() must compare against exactly this, not against the file as a
    whole: the rendered block appearing anywhere in the README is not the
    same claim as it being what the markers actually bracket, and a stale
    region hiding behind a correct-looking copy elsewhere is the one thing
    this check must never wave through.
    """
    start, end = _find_markers(text, lang)
    return text[start + len(MARKERS[lang][0]):end].strip("\n")


def buildspec_obs_version(text: str) -> str:
    """The OBS SDK the released binaries are built against, from buildspec.json."""
    try:
        version = json.loads(text)["dependencies"]["obs-studio"]["version"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise RangeError(f"buildspec.json has no dependencies.obs-studio.version ({error})")
    if not isinstance(version, str) or parse_version(version) is None:
        raise RangeError(f"buildspec.json's OBS version {version!r} is not a version tag")
    return version


def render_readme(readme: str, manifest: dict, built_against: str) -> str:
    """The README with every language block replaced; the rest left alone."""
    for lang in LANGS:
        readme = replace_between_markers(
            readme, render_readme_section(manifest, lang, built_against), lang)
    return readme


def write(root: Path = ROOT) -> None:
    """Render both README blocks from the manifest and buildspec.json.

    Unlike obs-playlist-deck, this never touches the pinned SDK version: moving
    buildspec.json's OBS version without its hashes and its obs-deps/Qt pins is
    how a plugin DLL fails to load with error 126. That bump is a manual step;
    what --write guarantees is that the README says what buildspec.json says.
    """
    manifest = load_manifest(root / "obs-compat.json")
    if manifest is None:
        raise RangeError(
            "no obs-compat.json to render from; run: python3 tools/obs_compat.py --report")
    built_against = buildspec_obs_version((root / BUILDSPEC_REL).read_text(encoding="utf-8"))

    # newline="\n": Python's text mode translates "\n" to "\r\n" on Windows,
    # which is where this repository is mostly edited, and a --write there
    # would leave a dirty tree whose diff shows no changed content.
    readme = root / "README.md"
    readme.write_text(
        render_readme(readme.read_text(encoding="utf-8"), manifest, built_against),
        encoding="utf-8", newline="\n")


def check(root: Path = ROOT) -> list[str]:
    """Offline consistency: both README blocks must match the manifest and buildspec."""
    manifest = load_manifest(root / "obs-compat.json")
    if manifest is None:
        return ["obs-compat.json is missing; run: python3 tools/obs_compat.py --report"]

    problems = []
    try:
        built_against = buildspec_obs_version(
            (root / BUILDSPEC_REL).read_text(encoding="utf-8"))
    except (RangeError, OSError) as error:
        return [f"cannot read the pinned OBS version: {error}"]

    readme = (root / "README.md").read_text(encoding="utf-8")
    for lang in LANGS:
        expected = render_readme_section(manifest, lang, built_against)
        try:
            region = _marker_region(readme, lang)
        except RangeError as exc:
            problems.append(f"{exc}; run: python3 tools/obs_compat.py --write")
            continue
        if region != expected:
            problems.append(
                f"README compatibility table ({lang}) does not match obs-compat.json "
                "and buildspec.json; run: python3 tools/obs_compat.py --write")
    return problems


# Below this, the runner's FFmpeg 7 cannot build OBS, so the probe moves into an
# ubuntu:22.04 container (FFmpeg 4.4). Lower this to FLOOR to disable the
# container path entirely — the older probes then report obs-build failures,
# which the range logic already treats as unverifiable rather than unsupported.
# With FLOOR at 32.0 nothing reaches the container today; it stays so that
# lowering FLOOR is a one-line change rather than a rebuild of the workflow.
LEGACY_BOUNDARY = (31, 0)

WEEKLY_CRON = "0 7 * * 1"
TAGS_URL = "https://api.github.com/repos/obsproject/obs-studio/tags?per_page=100"


def env_for(candidate: str) -> str:
    version = parse_version(candidate)
    return "jammy" if (version[0], version[1]) < LEGACY_BOUNDARY else "native"


def build_matrix(grid: list[str], latest_stable: str, beta: str | None,
                 manifest: dict | None) -> list[dict]:
    """One entry per probe, with the two gate candidates marked required.

    On the first run there is no manifest and therefore no declared minimum to
    defend, so only the newest stable gates. A minor that is in the grid but not
    yet in the manifest is never required either: that is how a freshly
    published OBS gets measured before it gets promised.
    """
    candidates = list(grid)
    if latest_stable not in candidates:
        candidates.append(latest_stable)
    if beta:
        candidates.append(beta)

    required = {latest_stable}
    if manifest:
        required.add(f"{manifest['min_supported']}.0")

    return [{"obs": candidate,
             "env": env_for(candidate),
             "required": candidate in required}
            for candidate in candidates]


def needs_full_run(event: str, schedule: str, ref: str, manifest: dict | None,
                   latest_stable: str, beta: str | None) -> bool:
    """The daily watch only wakes the matrix when OBS has actually moved."""
    if event == "workflow_dispatch" or (event == "push" and ref.startswith("refs/tags/")):
        return True
    if event == "schedule" and schedule == WEEKLY_CRON:
        return True
    if manifest is None:
        return True
    return (latest_stable != manifest.get("max_tested")
            or beta != manifest.get("beta_tested"))


# A runaway stop, not an expected ceiling: OBS is at ~250 tags today. Hitting
# it means the API paginated far more than any real tag list would, and
# returning a silently short list from here would produce a wrong grid, a
# wrong newest-stable, and a wrong README with nothing anywhere to say why.
TAG_CAP = 400


def fetch_tags(token: str | None) -> list[str]:
    """The only place this module touches the network."""
    request = urllib.request.Request(TAGS_URL, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "obs-multireplay-compat",
    })
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    tags: list[str] = []
    url: str | None = TAGS_URL
    while url:
        request.full_url = url
        with urllib.request.urlopen(request, timeout=30) as response:
            tags += [entry["name"] for entry in json.loads(response.read().decode("utf-8"))]
            link = response.headers.get("Link", "")
        url = None
        for part in link.split(","):
            if 'rel="next"' in part:
                url = part[part.find("<") + 1:part.find(">")]
        if url and len(tags) >= TAG_CAP:
            raise RangeError(
                f"the OBS tag list exceeded the {TAG_CAP}-tag cap in fetch_tags(); "
                "raise TAG_CAP")
    return tags


# Exit 1 means one thing and only one thing: the plugin does not compile
# against a probed OBS version. Nothing else -- not a bad artifact, not a
# missing argument, not an unexpected exception -- may ever produce it.
EXIT_OK = 0
EXIT_INCOMPATIBLE = 1
EXIT_STALE = 2
EXIT_NO_RANGE = 3
EXIT_BAD_INPUT = 4


def aggregate(artifact_dir: Path) -> tuple[dict[str, dict], list[str]]:
    """One result per probe artifact, plus the paths of any this run could not read.

    The probe job that produces these files is explicitly allowed to fail, so
    a truncated or half-written artifact is a realistic input, not a
    hypothetical. Omitting it from results is the semantically right answer,
    not a dodge: a version with no usable result is exactly what derive_range
    already classifies as unverifiable -- the same rule it applies to a probe
    that never reported at all. An unreadable result must never read as an
    incompatible one.

    The skipped list exists so a caller can tell a fully green run apart from
    a degraded one that merely looks green because some evidence went
    missing -- collapsing that distinction is what let --report claim "every
    probe is green" while quietly working from a smaller result set.
    """
    results: dict[str, dict] = {}
    skipped: list[str] = []
    for path in sorted(artifact_dir.glob("**/compat-*.json")):
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
            version = payload.pop("obs")
        except (json.JSONDecodeError, KeyError, OSError) as error:
            print(f"::warning::skipping unreadable artifact {path}: {error}", file=sys.stderr)
            skipped.append(str(path))
            continue
        results[version] = payload
    return results, skipped


def summary_table(manifest: dict) -> str:
    lines = [
        f"### OBS compatibility — {manifest['min_supported']} – {manifest['max_tested']}",
        "",
        "| version | env | status | phase |",
        "|---|---|---|---|",
    ]
    for version, result in manifest["results"].items():
        mark = "✅" if result["status"] == "ok" else "❌"
        lines.append(f"| `{version}` | {result['env']} | {mark} {result['status']} "
                     f"| {result['phase'] or '—'} |")
    return "\n".join(lines) + "\n"


def _emit_output(name: str, value: str) -> None:
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        # __EOF__ is a fixed delimiter, which would normally be a collision
        # risk. It is provably safe here: every value this function is ever
        # called with is a tag matched by _TAG (whose charset excludes
        # underscores) or JSON built from such tags, so __EOF__ can never
        # appear inside the value it delimits.
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(f"{name}<<__EOF__\n{value}\n__EOF__\n")
    else:
        print(f"{name}={value}")


def _discover() -> int:
    tags = fetch_tags(os.environ.get("GITHUB_TOKEN"))
    grid = select_grid(tags)
    latest = highest_stable(tags)
    beta = qualifying_beta(tags)
    manifest = load_manifest()
    full = needs_full_run(os.environ.get("GITHUB_EVENT_NAME", ""),
                          os.environ.get("GITHUB_SCHEDULE", ""),
                          os.environ.get("GITHUB_REF", ""),
                          manifest, latest, beta)
    matrix = build_matrix(grid, latest, beta, manifest)
    _emit_output("run_full", "true" if full else "false")
    _emit_output("grid", json.dumps(grid))
    _emit_output("latest_stable", latest)
    _emit_output("beta", beta or "")
    _emit_output("native", json.dumps([e for e in matrix if e["env"] == "native"]))
    _emit_output("jammy", json.dumps([e for e in matrix if e["env"] == "jammy"]))
    print(f"grid={grid} latest={latest} beta={beta} run_full={full}", file=sys.stderr)
    return EXIT_OK


# The three steps in order, because "run --write" on its own sent operators
# down a dead end: --write renders from the LOCAL obs-compat.json, which on
# their machine is still the old one, so it is a no-op and their local
# --check already passes. Nothing commits the manifest _report just wrote on
# the runner -- it only lands in the uploaded artifact -- so step 1 has to
# happen before --write does anything at all.
UPDATE_INSTRUCTIONS = (
    "To update the compatibility declaration: (1) download this run's "
    "'obs-compat-manifest' artifact and save it as obs-compat.json at the "
    "repository root, overwriting the committed copy; (2) run: "
    "python3 tools/obs_compat.py --write; (3) commit both files. Running "
    "--write against your local obs-compat.json first is a no-op -- it is "
    "still the old manifest until step 1 replaces it."
)


def _report(artifact_dir: Path, grid: list[str], latest: str, beta: str | None,
           root: Path = ROOT) -> int:
    """`root` is threaded through to save_manifest()/check() explicitly.

    Both bind a path default (MANIFEST_PATH, ROOT) at function-definition
    time, so patching the module-level attribute afterwards does not change
    an already-bound default argument -- the only way to redirect where the
    manifest is read from and written to is to pass the path down here.
    """
    results, skipped = aggregate(artifact_dir)
    # The beta is probed for forward-looking information only: it is never
    # in the declared range, never `required`, and the workflow already lets
    # its own job fail without failing the run (continue-on-error is keyed
    # off `required`). Gating the exit code on it here would fail the whole
    # run on a prerelease's behalf -- its failure still belongs in `results`
    # and the summary table, just not in this list.
    broken = [version for version, result in results.items()
              if version != beta and result.get("phase") == "plugin-build"]
    try:
        manifest = build_manifest(results, grid, latest, beta,
                                  datetime.date.today().isoformat())
    except RangeError as error:
        print(f"::error::{error}", file=sys.stderr)
        return EXIT_NO_RANGE

    save_manifest(manifest, root / "obs-compat.json")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(summary_table(manifest))
    else:
        table = summary_table(manifest)
        try:
            print(table)
        except UnicodeEncodeError:
            # print() encodes the whole joined string as one unit, so a
            # console that cannot represent the ✅/❌ marks (e.g. Windows'
            # cp1252) raises before a single byte reaches stdout -- the
            # entire table is lost, not just the two glyphs. Re-encoding
            # for that console with lossy substitution keeps every row in
            # front of the operator, marks degraded to '?', instead of
            # discarding the run's only human-readable summary.
            encoding = sys.stdout.encoding or "ascii"
            sys.stdout.buffer.write(table.encode(encoding, errors="replace"))
            sys.stdout.buffer.write(b"\n")

    if broken:
        print(f"::error::the plugin does not build against {', '.join(sorted(broken))}. "
              f"This is an incompatibility, not a CI failure.", file=sys.stderr)
        return EXIT_INCOMPATIBLE

    problems = check(root)
    for problem in problems:
        print(f"::error::{problem}", file=sys.stderr)
    if problems:
        # Determine if any probe failed at obs-build (SDK build failure).
        obs_build_failures = [version for version, result in results.items()
                               if result.get("phase") == "obs-build"]
        # A beta that failed is not "every probe is green" either -- its
        # result is real evidence and its absence from the declared range is
        # by design, but the message must not claim a clean sweep while
        # holding a fail/plugin-build record for it.
        beta_failed = beta is not None and results.get(beta, {}).get("status") != "ok"
        if skipped or obs_build_failures or beta_failed:
            # Do not claim every probe was green; some evidence is missing or
            # failed for a reason other than the plugin. Name whichever of
            # the three actually occurred rather than a fixed sentence that
            # would point at a zero count or at ::warning:: lines that were
            # never printed.
            reasons = []
            if skipped:
                reasons.append(f"{len(skipped)} artifact(s) could not be read "
                                f"(see ::warning:: messages above)")
            if obs_build_failures:
                reasons.append(f"the OBS SDK failed to build for "
                                f"{', '.join(sorted(obs_build_failures))}")
            if beta_failed:
                reasons.append(f"the beta ({beta}) failed to build")
            print(f"::notice::compatibility matrix check failed: {'; '.join(reasons)}. "
                  f"The supported range may not have genuinely moved — inspect "
                  f"--artifacts and re-run before updating the declaration. "
                  f"{UPDATE_INSTRUCTIONS}", file=sys.stderr)
        else:
            # All probes succeeded and check found problems → range simply moved.
            print(f"::notice::every probe is green — the declared range simply moved. "
                  f"{UPDATE_INSTRUCTIONS}", file=sys.stderr)
        return EXIT_STALE
    return EXIT_OK


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--discover", action="store_true")
    mode.add_argument("--report", action="store_true")
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    parser.add_argument("--artifacts", type=Path, default=ROOT / "compat-artifacts")
    parser.add_argument("--grid", help="JSON list, from the discover job")
    parser.add_argument("--latest-stable")
    parser.add_argument("--beta", default="")
    args = parser.parse_args()

    try:
        if args.discover:
            return _discover()
        if args.report:
            if not args.grid or not args.latest_stable:
                print("::error::--report requires --grid and --latest-stable", file=sys.stderr)
                return EXIT_BAD_INPUT
            try:
                grid = json.loads(args.grid)
            except json.JSONDecodeError as error:
                print(f"::error::--grid is not valid JSON: {error}", file=sys.stderr)
                return EXIT_BAD_INPUT
            return _report(args.artifacts, grid, args.latest_stable, args.beta or None)
        if args.write:
            write()
            print("ok: README now matches obs-compat.json and buildspec.json")
            return EXIT_OK
        problems = check()
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return EXIT_STALE if problems else EXIT_OK
    except RangeError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_NO_RANGE
    except Exception as error:
        # Nothing below this line may be allowed to fall through to Python's
        # own default exit status of 1 -- that number is reserved, in this
        # tool, for a genuine incompatibility, and an uncaught crash is not one.
        print(f"::error::unexpected {type(error).__name__}: {error}", file=sys.stderr)
        return EXIT_BAD_INPUT


if __name__ == "__main__":
    raise SystemExit(main())
