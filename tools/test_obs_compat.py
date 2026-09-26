#!/usr/bin/env python3
"""Unit tests for tools/obs_compat.py.

The compat matrix decides what the README promises, so its logic is tested
offline against a fixture of the OBS tag list rather than against the live API.
"""
from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import obs_compat

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "obs-tags.json"


def fixture_tags() -> list[str]:
    return [entry["name"] for entry in json.loads(FIXTURE.read_text(encoding="utf-8"))]


class ParseVersion(unittest.TestCase):
    def test_stable_tag_has_an_empty_suffix(self):
        self.assertEqual(obs_compat.parse_version("32.2.2"), (32, 2, 2, ""))

    def test_prerelease_keeps_its_suffix(self):
        self.assertEqual(obs_compat.parse_version("32.2.0-beta3"), (32, 2, 0, "beta3"))

    def test_non_version_tags_are_rejected(self):
        self.assertIsNone(obs_compat.parse_version("obs-studio-27.0.0"))
        self.assertIsNone(obs_compat.parse_version("latest"))

    def test_only_a_bare_x_y_z_counts_as_stable(self):
        self.assertTrue(obs_compat.is_stable(obs_compat.parse_version("32.2.2")))
        self.assertFalse(obs_compat.is_stable(obs_compat.parse_version("32.2.0-rc2")))


class Ordering(unittest.TestCase):
    def test_a_stable_release_outranks_its_own_prereleases(self):
        key = obs_compat.sort_key
        self.assertGreater(key(obs_compat.parse_version("32.2.0")),
                           key(obs_compat.parse_version("32.2.0-rc2")))

    def test_rc_outranks_beta_of_the_same_version(self):
        key = obs_compat.sort_key
        self.assertGreater(key(obs_compat.parse_version("32.2.0-rc2")),
                           key(obs_compat.parse_version("32.2.0-beta3")))

    def test_double_digit_prerelease_numbers_sort_numerically(self):
        key = obs_compat.sort_key
        self.assertGreater(key(obs_compat.parse_version("32.2.0-beta10")),
                           key(obs_compat.parse_version("32.2.0-beta9")))

    def test_rc10_sorts_above_rc2(self):
        key = obs_compat.sort_key
        self.assertGreater(key(obs_compat.parse_version("32.2.0-rc10")),
                           key(obs_compat.parse_version("32.2.0-rc2")))

    def test_stable_still_outranks_high_prerelease_numbers(self):
        key = obs_compat.sort_key
        self.assertGreater(key(obs_compat.parse_version("32.2.0")),
                           key(obs_compat.parse_version("32.2.0-rc10")))


class Grid(unittest.TestCase):
    def test_one_first_patch_per_minor_from_the_floor_up(self):
        self.assertEqual(
            obs_compat.select_grid(fixture_tags()),
            ["32.0.0", "32.1.0", "32.2.0"],
        )

    def test_versions_below_the_floor_are_excluded(self):
        grid = obs_compat.select_grid(fixture_tags())
        self.assertNotIn("31.1.0", grid)
        self.assertNotIn("30.0.0", grid)

    def test_a_minor_with_no_x_y_0_tag_is_skipped(self):
        # A minor whose .0 was never tagged cannot back the claim "X.Y+".
        self.assertEqual(obs_compat.select_grid(["32.0.1", "32.1.0"]), ["32.1.0"])


class Bounds(unittest.TestCase):
    def test_highest_stable_ignores_prereleases(self):
        self.assertEqual(obs_compat.highest_stable(fixture_tags()), "32.2.2")

    def test_an_all_prerelease_tag_list_raises_range_error_not_system_exit(self):
        # M1: SystemExit is not an Exception, so it would escape main()'s
        # catch-all and exit 1 -- the code reserved for "the plugin does not
        # compile against a probed OBS version". An empty stable-tag list
        # says nothing about the plugin at all, so it must be a RangeError.
        with self.assertRaises(obs_compat.RangeError):
            obs_compat.highest_stable(["32.3.0-beta1"])

    def test_no_beta_when_the_newest_prerelease_predates_the_newest_stable(self):
        self.assertIsNone(obs_compat.qualifying_beta(fixture_tags()))

    def test_a_beta_ahead_of_stable_qualifies(self):
        tags = fixture_tags() + ["32.3.0-beta1"]
        self.assertEqual(obs_compat.qualifying_beta(tags), "32.3.0-beta1")


def ok(env: str = "native") -> dict:
    return {"status": "ok", "phase": None, "env": env}


def incompatible(env: str = "native") -> dict:
    return {"status": "fail", "phase": "plugin-build", "env": env}


def unbuildable(env: str = "jammy") -> dict:
    return {"status": "fail", "phase": "obs-build", "env": env}


GRID = ["32.0.0", "32.1.0", "32.2.0"]

# The tag these fixtures use as max_tested throughout: a later patch of
# GRID's newest minor (32.2.0 vs 32.2.2), exactly the gap CRITICAL 1 is
# about. build_matrix probes it as its own candidate, separate from the
# grid, so a realistic `results` dict always carries a key for it too.
MAX_TESTED = "32.2.2"


# The OBS SDK buildspec.json pins for the released binaries. Deliberately not
# MAX_TESTED: in this repository the two are independent (see obs_compat's
# docstring), and a fixture that made them equal could not tell them apart.
BUILT_AGAINST = "31.1.1"


def write_buildspec(root: Path, version: str = BUILT_AGAINST) -> None:
    (root / "buildspec.json").write_text(
        json.dumps({"dependencies": {"obs-studio": {"version": version}}}),
        encoding="utf-8", newline="\n")


def readme_text(manifest: dict | None = None, built_against: str = BUILT_AGAINST) -> str:
    """A bilingual README fixture: both blocks stale, or both rendered from `manifest`."""
    def block(lang: str) -> str:
        start, end = obs_compat.MARKERS[lang]
        body = ("stale" if manifest is None
                else obs_compat.render_readme_section(manifest, lang, built_against))
        return "\n".join([start, body, end]) + "\n"
    return "\n".join([
        "## English", "", "### Requirements", "", block("en"), "### Installation", "",
        "## Italiano", "", "### Requisiti", "", block("it"), "### Installazione", "",
    ])


def grid_results() -> dict:
    """GRID plus MAX_TESTED, every entry green -- the all-clear baseline.

    Mirrors what a real fully-green run's `results` looks like: build_matrix
    always probes max_tested as a candidate distinct from the grid, so tests
    that construct `results` from GRID alone (as the old, buggy derive_range
    let them get away with) are testing a shape that never occurs in
    production.
    """
    results = {version: ok() for version in GRID}
    results[MAX_TESTED] = ok()
    return results


class DeriveRange(unittest.TestCase):
    def test_all_green_declares_the_floor(self):
        results = grid_results()
        derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
        self.assertEqual(derived["min_supported"], "32.0")
        self.assertEqual(derived["gaps"], [])
        self.assertEqual(derived["unverifiable"], [])

    def test_a_failing_middle_minor_raises_the_minimum(self):
        # 32.0 compiling does not make "32.0+" true when 32.1 does not.
        results = grid_results()
        results["32.1.0"] = incompatible()
        derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
        self.assertEqual(derived["min_supported"], "32.2")
        self.assertEqual(derived["gaps"], ["32.1"])

    def test_an_unbuildable_sdk_is_unverifiable_not_incompatible(self):
        results = grid_results()
        results["32.0.0"] = unbuildable()
        results["32.1.0"] = unbuildable()
        derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
        self.assertEqual(derived["min_supported"], "32.2")
        self.assertEqual(derived["unverifiable"], ["32.0", "32.1"])
        self.assertEqual(derived["gaps"], [])

    def test_a_missing_result_is_unverifiable_too(self):
        results = grid_results()
        del results["32.0.0"]
        derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
        self.assertEqual(derived["min_supported"], "32.1")
        self.assertEqual(derived["unverifiable"], ["32.0"])

    def test_the_block_must_reach_the_top_of_the_grid(self):
        # The newest minor failing means we cannot say what the range is at all.
        results = grid_results()
        results["32.2.0"] = incompatible()
        with self.assertRaises(obs_compat.RangeError):
            obs_compat.derive_range(results, GRID, MAX_TESTED)

    def test_only_failures_below_the_minimum_are_reported(self):
        results = grid_results()
        results["32.0.0"] = incompatible()
        derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
        self.assertEqual(derived["min_supported"], "32.1")
        self.assertEqual(derived["gaps"], ["32.0"])

    def test_max_tested_failing_at_obs_build_blocks_the_range(self):
        # CRITICAL 1: grid[-1] (32.2.0) is not max_tested (32.2.2) -- a later
        # patch of the same minor. The top of the grid being green must not
        # paper over max_tested itself being red.
        results = grid_results()
        results[MAX_TESTED] = unbuildable()
        with self.assertRaisesRegex(obs_compat.RangeError, re.escape(MAX_TESTED)):
            obs_compat.derive_range(results, GRID, MAX_TESTED)

    def test_max_tested_failing_at_plugin_build_does_not_block_derive_range(self):
        # The plugin failed to build against max_tested -- it is a known,
        # specific incompatibility that _report will report precisely as
        # EXIT_INCOMPATIBLE. Unlike an obs-build failure (the SDK would not
        # build), a plugin-build failure does not make the range undecidable.
        # derive_range must not raise; the broken check in _report is what
        # reports this finding.
        results = grid_results()
        results[MAX_TESTED] = incompatible()
        derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
        self.assertEqual(derived["min_supported"], "32.0")
        self.assertEqual(derived["gaps"], [])
        self.assertEqual(derived["unverifiable"], [])

    def test_the_declared_minimum_failing_at_obs_build_still_shifts_up(self):
        # Pin the related path that already works: an unbuildable SDK at the
        # declared floor becomes unverifiable, not incompatible, and the
        # minimum moves up past it -- this fix must not disturb that.
        results = grid_results()
        results["32.0.0"] = unbuildable()
        derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
        self.assertEqual(derived["min_supported"], "32.1")
        self.assertEqual(derived["unverifiable"], ["32.0"])
        self.assertEqual(derived["gaps"], [])


class Manifest(unittest.TestCase):
    def test_absent_manifest_reads_as_none_for_bootstrap(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(obs_compat.load_manifest(Path(tmp) / "obs-compat.json"))

    def test_a_saved_manifest_round_trips(self):
        results = grid_results()
        results["32.0.0"] = unbuildable()
        built = obs_compat.build_manifest(results, GRID, MAX_TESTED, None, "2026-08-31")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "obs-compat.json"
            obs_compat.save_manifest(built, path)
            self.assertEqual(obs_compat.load_manifest(path), built)

    def test_the_manifest_carries_the_range_and_the_floor_reason(self):
        built = obs_compat.build_manifest(
            grid_results(), GRID, MAX_TESTED, None, "2026-08-31")
        self.assertEqual(built["min_supported"], "32.0")
        self.assertEqual(built["max_tested"], "32.2.2")
        self.assertIsNone(built["beta_tested"])
        self.assertEqual(built["generated"], "2026-08-31")
        self.assertEqual(built["floor"]["version"], "32.0")
        self.assertIn("OBS 32", built["floor"]["reason"])

    def test_unverifiable_minors_are_kept_out_of_the_gaps(self):
        results = grid_results()
        results["32.0.0"] = unbuildable()
        built = obs_compat.build_manifest(results, GRID, MAX_TESTED, None, "2026-08-31")
        self.assertEqual(built["unverifiable"], ["32.0"])
        self.assertEqual(built["gaps"], [])
        self.assertEqual(built["results"]["32.0.0"]["phase"], "obs-build")

    def test_a_qualifying_beta_is_recorded_but_not_in_the_range(self):
        results = grid_results()
        results["32.3.0-beta1"] = ok()
        built = obs_compat.build_manifest(
            results, GRID, MAX_TESTED, "32.3.0-beta1", "2026-08-31")
        self.assertEqual(built["beta_tested"], "32.3.0-beta1")
        self.assertEqual(built["max_tested"], "32.2.2")


def sample_manifest(**overrides) -> dict:
    base = obs_compat.build_manifest(
        grid_results(), GRID, MAX_TESTED, None, "2026-08-31")
    base.update(overrides)
    return base


class ReadmeRendering(unittest.TestCase):
    def test_the_table_states_the_range_and_the_built_against_version(self):
        body = obs_compat.render_readme_section(sample_manifest())
        self.assertIn("**32.0 – 32.2.2**", body)
        self.assertIn("32.2.2", body)

    def test_the_table_says_the_check_is_compile_and_link(self):
        # Without this line "32.0+" promises more than the matrix verifies.
        body = obs_compat.render_readme_section(sample_manifest())
        self.assertIn("Compile and link", body)
        self.assertIn("not a runtime test", body)

    def test_unverifiable_minors_get_their_own_row(self):
        body = obs_compat.render_readme_section(
            sample_manifest(unverifiable=["32.0", "32.1"], min_supported="32.2"))
        self.assertIn("Not verifiable in CI", body)
        self.assertIn("32.0, 32.1", body)

    def test_no_unverifiable_row_when_there_is_nothing_to_report(self):
        self.assertNotIn("Not verifiable", obs_compat.render_readme_section(sample_manifest()))

    def test_a_beta_gets_its_own_row_and_stays_out_of_the_range(self):
        # A row is only earned by the beta's own probe result -- see the two
        # tests directly below -- so this one must supply a matching green
        # `results` entry rather than just setting `beta_tested` on its own,
        # which is not a shape production ever produces (build_manifest
        # always records a `results` entry for whatever it probed).
        results = grid_results()
        results["32.3.0-beta1"] = ok()
        body = obs_compat.render_readme_section(
            sample_manifest(beta_tested="32.3.0-beta1", results=results))
        self.assertIn("32.3.0-beta1", body)
        self.assertIn("**32.0 – 32.2.2**", body)

    def test_a_red_beta_is_shown_as_failing_and_never_claimed_as_supported(self):
        # CRITICAL 1: a red beta must never be advertised as something this
        # plugin "also builds against" -- that claim would contradict the
        # same manifest's own `results` entry for it.
        #
        # It must still APPEAR, though, marked as failing. The probe table is
        # evidence of what was measured, under a heading that says so; dropping
        # a measured failure from it would be the same silent omission this
        # whole feature exists to prevent, and a beta that breaks the plugin is
        # the most useful early warning the system can give. The claim is what
        # gets suppressed, not the fact.
        results = grid_results()
        results["32.3.0-beta1"] = incompatible()
        body = obs_compat.render_readme_section(
            sample_manifest(beta_tested="32.3.0-beta1", results=results))
        self.assertNotIn("Also builds against", body)
        beta_row = next(line for line in body.splitlines()
                        if "`32.3.0-beta1`" in line)
        self.assertIn("does not compile", beta_row)
        self.assertNotIn("Also builds against", body)

    def test_no_beta_row_when_the_beta_has_no_result_at_all(self):
        # beta_tested set but no matching results entry (an artifact that
        # never reported) is exactly as unproven as a red one -- absence of
        # evidence is not evidence of "ok", so this must not render either.
        body = obs_compat.render_readme_section(
            sample_manifest(beta_tested="32.3.0-beta1"))
        self.assertNotIn("32.3.0-beta1", body)
        self.assertNotIn("Also builds against", body)

    def test_the_static_rows_survive_generation(self):
        body = obs_compat.render_readme_section(sample_manifest())
        self.assertIn("Platforms", body)
        self.assertIn("Branch Output", body)

    def test_replacing_between_markers_leaves_the_rest_alone(self):
        text = f"before\n{obs_compat.README_START}\nold\n{obs_compat.README_END}\nafter\n"
        out = obs_compat.replace_between_markers(text, "new")
        self.assertIn("before", out)
        self.assertIn("after", out)
        self.assertIn("new", out)
        self.assertNotIn("old", out)

    def test_missing_markers_are_an_error_not_a_silent_no_op(self):
        with self.assertRaises(obs_compat.RangeError):
            obs_compat.replace_between_markers("no markers here", "new")


class ProbeTable(unittest.TestCase):
    """The range in the README is a conclusion; this table is its evidence."""

    def test_every_probed_version_is_listed(self):
        table = obs_compat.render_probe_table(sample_manifest())
        for version in GRID:
            self.assertIn(f"`{version}`", table)

    def test_versions_are_ordered_by_version_not_by_string(self):
        # "32.2.10" sorts before "32.2.2" as text; a reader scanning the table
        # for the newest row must not be handed the wrong one.
        manifest = sample_manifest()
        manifest["results"] = {
            "32.2.10": ok(), "32.2.2": ok(), "30.2.0": ok(), "31.0.0": ok(),
        }
        rows = [line for line in obs_compat.render_probe_table(manifest).splitlines()
                if line.startswith("| `")]
        self.assertEqual(
            [row.split("`")[1] for row in rows],
            ["30.2.0", "31.0.0", "32.2.2", "32.2.10"],
        )

    def test_an_unbuildable_sdk_does_not_read_as_an_incompatibility(self):
        # The distinction the whole feature exists for, in the one place a
        # non-contributor actually reads.
        manifest = sample_manifest()
        manifest["results"]["32.0.0"] = unbuildable()
        row = next(line for line in obs_compat.render_probe_table(manifest).splitlines()
                   if "`32.0.0`" in line)
        self.assertIn("could not be built", row)
        self.assertNotIn("does not compile", row)

    def test_a_real_incompatibility_says_so(self):
        manifest = sample_manifest()
        manifest["results"]["31.0.0"] = incompatible()
        row = next(line for line in obs_compat.render_probe_table(manifest).splitlines()
                   if "`31.0.0`" in line)
        self.assertIn("does not compile", row)
        self.assertNotIn("could not be built", row)

    def test_environments_are_named_for_the_reader_not_for_the_workflow(self):
        manifest = sample_manifest()
        manifest["results"]["32.0.0"] = ok("jammy")
        table = obs_compat.render_probe_table(manifest)
        self.assertIn("Ubuntu 22.04", table)
        self.assertIn("Ubuntu 24.04", table)
        self.assertNotIn("jammy", table)

    def test_the_readme_section_carries_the_table_and_points_at_the_manifest(self):
        body = obs_compat.render_readme_section(sample_manifest())
        self.assertIn("`32.0.0`", body)
        self.assertIn("obs-compat.json", body)
        # Still a conclusion first, evidence second.
        self.assertLess(body.index("**32.0 – 32.2.2**"), body.index("`32.0.0`"))


class LineEndings(unittest.TestCase):
    def test_write_never_produces_crlf(self):
        # Python's text mode translates "\n" to "\r\n" on Windows, so a --write
        # there would rewrite the README with CRLF and leave a maintainer staring
        # at a dirty tree whose diff shows no changed content. This assertion is
        # a no-op on Linux CI and the whole point of the test on Windows.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            write_buildspec(root)
            (root / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")
            obs_compat.save_manifest(sample_manifest(), root / "obs-compat.json")
            obs_compat.write(root)

            for name_ in ("README.md", "obs-compat.json"):
                with self.subTest(file=name_):
                    self.assertNotIn(b"\r\n", (root / name_).read_bytes())


class BuildspecVersion(unittest.TestCase):
    def test_it_reads_the_pinned_obs_version(self):
        text = json.dumps({"dependencies": {"obs-studio": {"version": "31.1.1"},
                                            "qt6": {"version": "2026-06-02"}}})
        self.assertEqual(obs_compat.buildspec_obs_version(text), "31.1.1")

    def test_a_buildspec_without_it_is_an_error_not_a_silent_blank(self):
        with self.assertRaises(obs_compat.RangeError):
            obs_compat.buildspec_obs_version(json.dumps({"dependencies": {}}))

    def test_a_non_version_is_rejected(self):
        with self.assertRaises(obs_compat.RangeError):
            obs_compat.buildspec_obs_version(
                json.dumps({"dependencies": {"obs-studio": {"version": "latest"}}}))


class BilingualReadme(unittest.TestCase):
    def test_both_languages_state_the_same_range(self):
        for lang in obs_compat.LANGS:
            with self.subTest(lang=lang):
                body = obs_compat.render_readme_section(sample_manifest(), lang, BUILT_AGAINST)
                self.assertIn("**32.0 – 32.2.2**", body)
                self.assertIn(BUILT_AGAINST, body)

    def test_the_italian_block_is_in_italian(self):
        body = obs_compat.render_readme_section(sample_manifest(), "it", BUILT_AGAINST)
        self.assertIn("Compilato contro", body)
        self.assertIn("non è un test a runtime", body)
        self.assertIn("Piattaforme", body)
        self.assertNotIn("Platforms", body)

    def test_the_built_against_row_is_buildspec_not_max_tested(self):
        body = obs_compat.render_readme_section(sample_manifest(), "en", "31.1.1")
        row = next(line for line in body.splitlines() if "Built against" in line)
        self.assertIn("31.1.1", row)
        self.assertNotIn(MAX_TESTED, row)

    def test_write_fills_both_blocks_and_leaves_the_rest_alone(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            write_buildspec(root)
            (root / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")
            obs_compat.save_manifest(sample_manifest(), root / "obs-compat.json")
            obs_compat.write(root)
            text = (root / "README.md").read_text(encoding="utf-8")
            self.assertNotIn("stale", text)
            self.assertIn("### Installation", text)
            self.assertIn("### Installazione", text)
            self.assertIn("Every OBS version CI probed", text)
            self.assertIn("Tutte le versioni di OBS provate dalla CI", text)


class WriteThenCheck(unittest.TestCase):
    def _repo(self, tmp: Path, built_against: str = BUILT_AGAINST) -> Path:
        write_buildspec(tmp, built_against)
        (tmp / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")
        obs_compat.save_manifest(sample_manifest(), tmp / "obs-compat.json")
        return tmp

    def test_write_then_check_is_clean(self):
        with tempfile.TemporaryDirectory() as name:
            root = self._repo(Path(name))
            obs_compat.write(root)
            self.assertEqual(obs_compat.check(root), [])

    def test_check_flags_a_stale_readme(self):
        with tempfile.TemporaryDirectory() as name:
            root = self._repo(Path(name))
            problems = obs_compat.check(root)
            self.assertTrue(any("README" in problem for problem in problems))

    def test_check_flags_each_stale_language_on_its_own(self):
        # Rendering only one language must not satisfy the check for both.
        with tempfile.TemporaryDirectory() as name:
            root = self._repo(Path(name))
            readme = root / "README.md"
            readme.write_text(obs_compat.replace_between_markers(
                readme.read_text(encoding="utf-8"),
                obs_compat.render_readme_section(sample_manifest(), "en", BUILT_AGAINST),
                "en"), encoding="utf-8", newline="\n")
            problems = obs_compat.check(root)
            self.assertEqual(len(problems), 1)
            self.assertIn("(it)", problems[0])

    def test_check_flags_a_buildspec_bump_the_readme_does_not_know_about(self):
        # buildspec.json moves by hand (hashes, obs-deps, Qt); the README row
        # naming it must not be allowed to keep the old number.
        with tempfile.TemporaryDirectory() as name:
            root = self._repo(Path(name))
            obs_compat.write(root)
            write_buildspec(root, "32.2.2")
            problems = obs_compat.check(root)
            self.assertTrue(any("buildspec" in problem for problem in problems))

    def test_check_reports_a_missing_manifest_rather_than_crashing(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / "README.md").write_text("nothing", encoding="utf-8")
            self.assertTrue(any("obs-compat.json" in problem for problem in obs_compat.check(root)))

    def test_check_reports_a_missing_buildspec_rather_than_crashing(self):
        with tempfile.TemporaryDirectory() as name:
            root = self._repo(Path(name))
            (root / "buildspec.json").unlink()
            self.assertTrue(any("OBS version" in problem for problem in obs_compat.check(root)))

    def test_check_is_not_fooled_by_a_correct_block_sitting_outside_the_markers(self):
        # A correct copy anywhere in the file is not the same claim as the
        # marker region itself being correct; only the latter is what --write
        # actually produces, and only the latter is what --check must trust.
        with tempfile.TemporaryDirectory() as name:
            root = self._repo(Path(name))
            readme = root / "README.md"
            extra = "".join(
                "\n" + obs_compat.render_readme_section(sample_manifest(), lang, BUILT_AGAINST)
                + "\n" for lang in obs_compat.LANGS)
            readme.write_text(readme.read_text(encoding="utf-8") + extra, encoding="utf-8")
            problems = obs_compat.check(root)
            self.assertTrue(any("README" in problem for problem in problems))

    def test_write_raises_rather_than_render_half_a_readme(self):
        # A README missing one language's markers must stop --write, not leave
        # English regenerated and Italian quietly as it was.
        with tempfile.TemporaryDirectory() as name:
            root = self._repo(Path(name))
            start, _end = obs_compat.MARKERS["it"]
            readme = root / "README.md"
            readme.write_text(readme.read_text(encoding="utf-8").replace(start, ""),
                              encoding="utf-8")
            before = readme.read_text(encoding="utf-8")
            with self.assertRaises(obs_compat.RangeError):
                obs_compat.write(root)
            self.assertEqual(readme.read_text(encoding="utf-8"), before)


class Environment(unittest.TestCase):
    def test_pre_31_probes_run_in_the_jammy_container(self):
        # ubuntu-24.04 ships FFmpeg 7, which OBS 30-era code will not build against.
        self.assertEqual(obs_compat.env_for("30.2.0"), "jammy")

    def test_31_and_later_run_on_the_native_runner(self):
        self.assertEqual(obs_compat.env_for("31.0.0"), "native")
        self.assertEqual(obs_compat.env_for("32.2.2"), "native")


class BuildMatrix(unittest.TestCase):
    def test_every_grid_candidate_plus_the_newest_stable_is_probed(self):
        matrix = obs_compat.build_matrix(GRID, "32.2.2", None, sample_manifest())
        self.assertEqual([entry["obs"] for entry in matrix], GRID + ["32.2.2"])

    def test_the_newest_stable_is_not_duplicated_when_it_is_already_x_y_0(self):
        matrix = obs_compat.build_matrix(GRID, "32.2.0", None, sample_manifest())
        self.assertEqual([entry["obs"] for entry in matrix].count("32.2.0"), 1)

    def test_exactly_the_two_extremes_are_required(self):
        matrix = obs_compat.build_matrix(GRID, "32.2.2", None, sample_manifest())
        required = [entry["obs"] for entry in matrix if entry["required"]]
        self.assertEqual(required, ["32.0.0", "32.2.2"])

    def test_the_declared_minimum_is_probed_at_its_first_patch(self):
        # The manifest stores a minor; the grid probes X.Y.0.
        matrix = obs_compat.build_matrix(
            GRID, "32.2.2", None, sample_manifest(min_supported="32.1"))
        required = [entry["obs"] for entry in matrix if entry["required"]]
        self.assertEqual(required, ["32.1.0", "32.2.2"])

    def test_a_qualifying_beta_is_probed_but_never_required(self):
        matrix = obs_compat.build_matrix(GRID, "32.2.2", "32.3.0-beta1", sample_manifest())
        beta = [entry for entry in matrix if entry["obs"] == "32.3.0-beta1"]
        self.assertEqual(len(beta), 1)
        self.assertFalse(beta[0]["required"])
        self.assertEqual(beta[0]["env"], "native")

    def test_bootstrap_requires_only_the_newest_stable(self):
        matrix = obs_compat.build_matrix(GRID, "32.2.2", None, None)
        required = [entry["obs"] for entry in matrix if entry["required"]]
        self.assertEqual(required, ["32.2.2"])


class NeedsFullRun(unittest.TestCase):
    def test_a_tag_push_always_runs_the_full_matrix(self):
        self.assertTrue(obs_compat.needs_full_run(
            "push", "", "refs/tags/v1.3.2", sample_manifest(), "32.2.2", None))

    def test_a_branch_push_with_an_unchanged_manifest_does_not_run_the_full_matrix(self):
        # This is what the watcher design rests on. If the push/tag check is
        # ever "simplified" to fire on any push rather than only a tag push,
        # this test is what catches it — otherwise every push to main would
        # start building OBS from source.
        self.assertFalse(obs_compat.needs_full_run(
            "push", "", "refs/heads/main", sample_manifest(), "32.2.2", None))

    def test_a_manual_dispatch_always_runs_the_full_matrix(self):
        self.assertTrue(obs_compat.needs_full_run(
            "workflow_dispatch", "", "refs/heads/main", sample_manifest(), "32.2.2", None))

    def test_the_weekly_cron_always_runs_the_full_matrix(self):
        self.assertTrue(obs_compat.needs_full_run(
            "schedule", obs_compat.WEEKLY_CRON, "refs/heads/main",
            sample_manifest(), "32.2.2", None))

    def test_the_daily_watch_stays_quiet_when_obs_has_not_moved(self):
        self.assertFalse(obs_compat.needs_full_run(
            "schedule", "0 6 * * *", "refs/heads/main", sample_manifest(), "32.2.2", None))

    def test_the_daily_watch_fires_when_obs_ships_a_stable(self):
        self.assertTrue(obs_compat.needs_full_run(
            "schedule", "0 6 * * *", "refs/heads/main", sample_manifest(), "32.3.0", None))

    def test_the_daily_watch_fires_when_a_beta_appears(self):
        self.assertTrue(obs_compat.needs_full_run(
            "schedule", "0 6 * * *", "refs/heads/main",
            sample_manifest(), "32.2.2", "32.3.0-beta1"))

    def test_bootstrap_runs_the_full_matrix(self):
        self.assertTrue(obs_compat.needs_full_run(
            "schedule", "0 6 * * *", "refs/heads/main", None, "32.2.2", None))


class _FakePage:
    """Stands in for the `urllib.request.urlopen` context manager's response.

    `link` is the raw `Link` header value fetch_tags() parses for pagination;
    an empty string reproduces a response with no Link header at all.
    """
    def __init__(self, tags: list[str], link: str = ""):
        self._body = json.dumps([{"name": tag} for tag in tags]).encode("utf-8")
        self._link = link

    def read(self) -> bytes:
        return self._body

    @property
    def headers(self) -> dict:
        return {"Link": self._link} if self._link else {}

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        return False


def _fake_urlopen(pages: list["_FakePage"]):
    """Serves `pages` in order, one per fetch_tags() request, ignoring the URL."""
    remaining = iter(pages)

    def _urlopen(request, timeout=30):
        return next(remaining)

    return _urlopen


def _next_link(url: str) -> str:
    return f'<{url}>; rel="next"'


class FetchTagsPagination(unittest.TestCase):
    def test_hitting_the_cap_raises_instead_of_truncating(self):
        # Four pages of 100 land exactly on TAG_CAP (400) while a next link is
        # still present — a silent truncation here would ship a short tag
        # list and, downstream, a wrong grid and a wrong README.
        pages = [
            _FakePage([f"30.{i}.0" for i in range(100)], _next_link("https://x/2")),
            _FakePage([f"31.{i}.0" for i in range(100)], _next_link("https://x/3")),
            _FakePage([f"32.{i}.0" for i in range(100)], _next_link("https://x/4")),
            _FakePage([f"33.{i}.0" for i in range(100)], _next_link("https://x/5")),
        ]
        with mock.patch.object(obs_compat.urllib.request, "urlopen", _fake_urlopen(pages)):
            with self.assertRaises(obs_compat.RangeError) as ctx:
                obs_compat.fetch_tags(None)
        self.assertIn("cap", str(ctx.exception))

    def test_a_response_under_the_cap_never_raises(self):
        pages = [_FakePage(["32.0.0", "32.1.0"])]
        with mock.patch.object(obs_compat.urllib.request, "urlopen", _fake_urlopen(pages)):
            tags = obs_compat.fetch_tags(None)
        self.assertEqual(tags, ["32.0.0", "32.1.0"])

    def test_a_missing_link_header_terminates_pagination_cleanly(self):
        pages = [_FakePage(["32.0.0", "32.1.0"], link="")]
        with mock.patch.object(obs_compat.urllib.request, "urlopen", _fake_urlopen(pages)):
            tags = obs_compat.fetch_tags(None)
        self.assertEqual(tags, ["32.0.0", "32.1.0"])

    def test_a_malformed_link_header_terminates_pagination_cleanly(self):
        # A Link header present but with no rel="next" part (e.g. only
        # rel="prev") must read the same as no Link header at all.
        pages = [_FakePage(["32.0.0"], link='<https://x/0>; rel="prev"')]
        with mock.patch.object(obs_compat.urllib.request, "urlopen", _fake_urlopen(pages)):
            tags = obs_compat.fetch_tags(None)
        self.assertEqual(tags, ["32.0.0"])


class Aggregate(unittest.TestCase):
    def test_it_reads_one_file_per_probe_from_nested_artifact_dirs(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for version, payload in (("32.0.0", unbuildable()), ("32.2.2", ok())):
                folder = root / f"compat-{version}"
                folder.mkdir()
                (folder / f"compat-{version}.json").write_text(
                    json.dumps({"obs": version, **payload}), encoding="utf-8")
            results, skipped = obs_compat.aggregate(root)
            self.assertEqual(results, {"32.0.0": unbuildable(), "32.2.2": ok()})
            self.assertEqual(skipped, [])

    def test_an_empty_artifact_dir_aggregates_to_nothing(self):
        with tempfile.TemporaryDirectory() as name:
            results, skipped = obs_compat.aggregate(Path(name))
            self.assertEqual(results, {})
            self.assertEqual(skipped, [])

    def test_an_empty_artifact_file_is_skipped_not_fatal(self):
        # The probe job that writes these files is explicitly allowed to
        # fail, so a truncated (here: zero-byte) artifact is realistic input.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            broken = root / "compat-32.0.0"
            broken.mkdir()
            (broken / "compat-32.0.0.json").write_text("", encoding="utf-8")
            good = root / "compat-32.2.2"
            good.mkdir()
            (good / "compat-32.2.2.json").write_text(
                json.dumps({"obs": "32.2.2", **ok()}), encoding="utf-8")
            results, skipped = obs_compat.aggregate(root)
            self.assertEqual(results, {"32.2.2": ok()})
            self.assertEqual(len(skipped), 1)

    def test_a_malformed_artifact_file_is_skipped_not_fatal(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            broken = root / "compat-32.0.0"
            broken.mkdir()
            (broken / "compat-32.0.0.json").write_text("{not valid json", encoding="utf-8")
            good = root / "compat-32.2.2"
            good.mkdir()
            (good / "compat-32.2.2.json").write_text(
                json.dumps({"obs": "32.2.2", **ok()}), encoding="utf-8")
            results, skipped = obs_compat.aggregate(root)
            self.assertEqual(results, {"32.2.2": ok()})
            self.assertEqual(len(skipped), 1)

    def test_an_artifact_missing_the_obs_key_is_skipped_not_fatal(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            broken = root / "compat-32.0.0"
            broken.mkdir()
            (broken / "compat-32.0.0.json").write_text(json.dumps(ok()), encoding="utf-8")
            good = root / "compat-32.2.2"
            good.mkdir()
            (good / "compat-32.2.2.json").write_text(
                json.dumps({"obs": "32.2.2", **ok()}), encoding="utf-8")
            results, skipped = obs_compat.aggregate(root)
            self.assertEqual(results, {"32.2.2": ok()})
            self.assertEqual(len(skipped), 1)

    def test_a_version_with_an_unreadable_artifact_is_unverifiable_not_a_gap(self):
        # This is the semantic the fix exists to pin: an unreadable result
        # must read exactly like a probe that never reported, never like a
        # confirmed incompatibility.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for version in GRID:
                folder = root / f"compat-{version}"
                folder.mkdir()
                if version == "32.0.0":
                    (folder / f"compat-{version}.json").write_text(
                        "{not valid json", encoding="utf-8")
                else:
                    (folder / f"compat-{version}.json").write_text(
                        json.dumps({"obs": version, **ok()}), encoding="utf-8")
            # max_tested is probed as its own matrix entry, separate from
            # GRID's X.Y.0 candidates -- write it too, green, so it does not
            # itself trip the max_tested-must-be-green gate this test is not
            # about.
            extra = root / f"compat-{MAX_TESTED}"
            extra.mkdir()
            (extra / f"compat-{MAX_TESTED}.json").write_text(
                json.dumps({"obs": MAX_TESTED, **ok()}), encoding="utf-8")
            results, skipped = obs_compat.aggregate(root)
            self.assertEqual(len(skipped), 1)
            derived = obs_compat.derive_range(results, GRID, MAX_TESTED)
            self.assertEqual(derived["unverifiable"], ["32.0"])
            self.assertEqual(derived["gaps"], [])


class SummaryTable(unittest.TestCase):
    def test_it_names_the_phase_of_every_failure(self):
        manifest = sample_manifest()
        manifest["results"]["32.0.0"] = unbuildable()
        table = obs_compat.summary_table(manifest)
        self.assertIn("32.0.0", table)
        self.assertIn("obs-build", table)


class ExitCodes(unittest.TestCase):
    def test_the_codes_are_distinct(self):
        codes = {obs_compat.EXIT_OK, obs_compat.EXIT_INCOMPATIBLE,
                 obs_compat.EXIT_STALE, obs_compat.EXIT_NO_RANGE,
                 obs_compat.EXIT_BAD_INPUT}
        self.assertEqual(len(codes), 5)


class ReportMessageWhenDegraded(unittest.TestCase):
    """Verify the _report message accurately reflects when evidence is missing.

    Each test passes its own temp `root` straight into _report() (rather than
    patching the obs_compat.ROOT module attribute, which check() and
    save_manifest() never re-read because their path defaults are bound at
    function-definition time) so that _report reads and writes only the
    fixture README/workflow/manifest built here -- never the real repository.
    """

    def test_report_with_skipped_artifact_does_not_claim_all_green(self):
        # When an artifact is unreadable, do not claim every probe was green.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            # Set up a mock repo
            write_buildspec(root)
            (root / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")

            # Create artifact dir with all versions but 32.0.0 unreadable
            artifact_dir = root / "compat-artifacts"
            artifact_dir.mkdir()
            for version in GRID:
                folder = artifact_dir / f"compat-{version}"
                folder.mkdir()
                if version == "32.0.0":
                    # Create an unreadable artifact
                    (folder / f"compat-{version}.json").write_text("", encoding="utf-8")
                else:
                    (folder / f"compat-{version}.json").write_text(
                        json.dumps({"obs": version, **ok()}), encoding="utf-8")
            # max_tested is probed as its own matrix entry, separate from the
            # grid -- write it too, green, since this test is about a
            # skipped artifact elsewhere, not about the max_tested gate.
            extra = artifact_dir / f"compat-{MAX_TESTED}"
            extra.mkdir()
            (extra / f"compat-{MAX_TESTED}.json").write_text(
                json.dumps({"obs": MAX_TESTED, **ok()}), encoding="utf-8")

            # Create an initial manifest so check() will find differences
            obs_compat.save_manifest(sample_manifest(), root / "obs-compat.json")

            # Set GITHUB_STEP_SUMMARY to a temp file (hermetic, real CI path, UTF-8)
            summary_file = root / "summary.txt"
            with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary_file)}):
                import io
                captured_stderr = io.StringIO()
                with mock.patch("sys.stderr", captured_stderr):
                    exit_code = obs_compat._report(artifact_dir, GRID, MAX_TESTED, None, root=root)

            self.assertEqual(exit_code, obs_compat.EXIT_STALE)
            stderr = captured_stderr.getvalue()
            # Should mention artifacts couldn't be read
            self.assertIn("artifact(s) could not be read", stderr)
            # Should NOT claim every probe was green
            self.assertNotIn("every probe is green", stderr)

            # Proof this consulted the fixture, not the real repo: the
            # initial manifest written above declared 32.0.0 ok and
            # unverifiable == []. Only a run against *this* artifact_dir and
            # *this* root produces 32.0.0 missing from results and "32.0"
            # promoted to unverifiable -- a run against the real repository
            # root could not have rewritten this file at all.
            written = obs_compat.load_manifest(root / "obs-compat.json")
            self.assertNotIn("32.0.0", written["results"])
            self.assertEqual(written["unverifiable"], ["32.0"])

    def test_report_with_obs_build_failure_does_not_claim_all_green(self):
        # When a probe failed at obs-build, do not claim every probe was green.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            # Set up a mock repo
            write_buildspec(root)
            (root / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")

            # Create artifact dir with all versions but 32.0.0 has obs-build failure
            artifact_dir = root / "compat-artifacts"
            artifact_dir.mkdir()
            for version in GRID:
                folder = artifact_dir / f"compat-{version}"
                folder.mkdir()
                if version == "32.0.0":
                    (folder / f"compat-{version}.json").write_text(
                        json.dumps({"obs": version, **unbuildable()}), encoding="utf-8")
                else:
                    (folder / f"compat-{version}.json").write_text(
                        json.dumps({"obs": version, **ok()}), encoding="utf-8")
            # max_tested is probed as its own matrix entry, separate from the
            # grid -- write it too, green, since this test's obs-build
            # failure is deliberately at the declared minimum, not at
            # max_tested.
            extra = artifact_dir / f"compat-{MAX_TESTED}"
            extra.mkdir()
            (extra / f"compat-{MAX_TESTED}.json").write_text(
                json.dumps({"obs": MAX_TESTED, **ok()}), encoding="utf-8")

            # Create an initial manifest so check() will find differences
            obs_compat.save_manifest(sample_manifest(), root / "obs-compat.json")

            # Set GITHUB_STEP_SUMMARY to a temp file (hermetic, real CI path, UTF-8)
            summary_file = root / "summary.txt"
            with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary_file)}):
                import io
                captured_stderr = io.StringIO()
                with mock.patch("sys.stderr", captured_stderr):
                    exit_code = obs_compat._report(artifact_dir, GRID, MAX_TESTED, None, root=root)

            self.assertEqual(exit_code, obs_compat.EXIT_STALE)
            stderr = captured_stderr.getvalue()
            # Should NOT claim every probe was green (because obs-build failures exist)
            self.assertNotIn("every probe is green", stderr)

            # Proof this consulted the fixture: 32.0.0's obs-build failure is
            # only visible in results/unverifiable if the manifest was
            # rebuilt from this test's own artifact_dir and written back to
            # this test's own root.
            written = obs_compat.load_manifest(root / "obs-compat.json")
            self.assertEqual(written["results"]["32.0.0"]["phase"], "obs-build")
            self.assertEqual(written["unverifiable"], ["32.0"])
            # Pin the behaviour this fix must not disturb: the declared
            # minimum failing at obs-build still exits 2 (EXIT_STALE), and
            # the range genuinely shifts up past it rather than staying put.
            self.assertEqual(written["min_supported"], "32.1")

    def test_report_fully_green_stale_run_claims_probes_are_green(self):
        # When all probes succeeded and nothing was skipped, the original message is used.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            # Set up a mock repo
            write_buildspec(root)
            (root / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")

            # Create artifact dir with all good artifacts
            artifact_dir = root / "compat-artifacts"
            artifact_dir.mkdir()
            for version in GRID:
                folder = artifact_dir / f"compat-{version}"
                folder.mkdir()
                (folder / f"compat-{version}.json").write_text(
                    json.dumps({"obs": version, **ok()}), encoding="utf-8")
            # max_tested is probed as its own matrix entry, separate from the
            # grid -- write it too, green, since this is the fully-green case.
            extra = artifact_dir / f"compat-{MAX_TESTED}"
            extra.mkdir()
            (extra / f"compat-{MAX_TESTED}.json").write_text(
                json.dumps({"obs": MAX_TESTED, **ok()}), encoding="utf-8")

            # Create an initial manifest so check() will find differences
            obs_compat.save_manifest(sample_manifest(), root / "obs-compat.json")

            # Set GITHUB_STEP_SUMMARY to a temp file (hermetic, real CI path, UTF-8)
            summary_file = root / "summary.txt"
            with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary_file)}):
                import io
                captured_stderr = io.StringIO()
                with mock.patch("sys.stderr", captured_stderr):
                    exit_code = obs_compat._report(artifact_dir, GRID, MAX_TESTED, None, root=root)

            self.assertEqual(exit_code, obs_compat.EXIT_STALE)
            stderr = captured_stderr.getvalue()
            # Should claim every probe is green
            self.assertIn("every probe is green", stderr)
            # Should NOT mention artifacts that couldn't be read
            self.assertNotIn("artifact(s) could not be read", stderr)

            # Proof this consulted the fixture: the summary file this test
            # pointed GITHUB_STEP_SUMMARY at is the only place the table
            # could have been written, since check(root) found the fixture
            # README stale (real content would never contain "stale").
            self.assertIn("32.2.2", summary_file.read_text(encoding="utf-8"))
            written = obs_compat.load_manifest(root / "obs-compat.json")
            self.assertEqual(written["unverifiable"], [])
            self.assertIn("32.0.0", written["results"])


class ReportDegradesAnUnencodableSummaryInsteadOfDroppingIt(unittest.TestCase):
    """The stdout fallback must not silently swallow the whole table.

    print() encodes the joined table string as one unit, so a console that
    cannot represent the ✅/❌ marks (e.g. Windows' cp1252) used to raise
    UnicodeEncodeError before a single byte reached stdout -- the entire
    table was lost, not just the two glyphs, and the old `except: pass`
    hid that. This is the one branch none of the ReportMessageWhenDegraded
    tests reaches, because all three set GITHUB_STEP_SUMMARY and take the
    file-write path instead.
    """

    def test_every_row_still_reaches_the_operator_with_marks_degraded(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            write_buildspec(root)
            (root / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")

            artifact_dir = root / "compat-artifacts"
            artifact_dir.mkdir()
            for version in GRID:
                folder = artifact_dir / f"compat-{version}"
                folder.mkdir()
                (folder / f"compat-{version}.json").write_text(
                    json.dumps({"obs": version, **ok()}), encoding="utf-8")
            # max_tested is probed as its own matrix entry, separate from
            # the grid -- write it too, green, so build_manifest can derive
            # a range at all; this test is about the print() fallback, not
            # about the max_tested gate.
            extra = artifact_dir / f"compat-{MAX_TESTED}"
            extra.mkdir()
            (extra / f"compat-{MAX_TESTED}.json").write_text(
                json.dumps({"obs": MAX_TESTED, **ok()}), encoding="utf-8")

            import io

            class ConsoleThatCannotEncodeTheMarks:
                """Stands in for a real cp1252 console: write() raises
                exactly the way the real one does on a string containing
                ✅/❌, and whatever the degrade path writes to .buffer
                afterward is what actually reached the operator."""
                encoding = "cp1252"

                def __init__(self):
                    self.buffer = io.BytesIO()

                def write(self, text):
                    text.encode(self.encoding)

                def flush(self):
                    pass

            fake_stdout = ConsoleThatCannotEncodeTheMarks()

            # No GITHUB_STEP_SUMMARY: forces _report onto the print()
            # fallback branch rather than the file-write branch the other
            # three tests exercise. clear the key even if the ambient
            # environment (e.g. a real GitHub Actions runner) set it.
            with mock.patch.dict(os.environ, {}, clear=False):
                os.environ.pop("GITHUB_STEP_SUMMARY", None)
                with mock.patch("sys.stdout", fake_stdout):
                    with mock.patch("sys.stderr", io.StringIO()):
                        obs_compat._report(artifact_dir, GRID, MAX_TESTED, None, root=root)

            written = fake_stdout.buffer.getvalue().decode("cp1252")
            # Every row reached the operator -- the whole table survived,
            # not just an empty write.
            for version in GRID:
                self.assertIn(version, written)
            self.assertIn("ok", written)
            # The unencodable ✅ marks degraded to a substitute character
            # rather than the row (or the whole print) being dropped.
            self.assertNotIn("✅", written)
            self.assertGreaterEqual(written.count("?"), len(GRID))


class ReportGateChecksMaxTestedItself(unittest.TestCase):
    """CRITICAL 1, at the --report boundary: grid[-1] (32.2.0) is not
    max_tested (32.2.2) whenever a later patch of the newest minor has
    shipped, and it is max_tested -- not grid[-1] -- that build_matrix marks
    `required` and that the README calls "Built against". A required probe
    failing at max_tested must block the release, not slip through because
    the grid's own top happened to be green.
    """

    def _artifact_dir(self, root: Path, max_tested_result: dict) -> Path:
        artifact_dir = root / "compat-artifacts"
        artifact_dir.mkdir()
        for version in GRID:
            folder = artifact_dir / f"compat-{version}"
            folder.mkdir()
            (folder / f"compat-{version}.json").write_text(
                json.dumps({"obs": version, **ok()}), encoding="utf-8")
        extra = artifact_dir / f"compat-{MAX_TESTED}"
        extra.mkdir()
        (extra / f"compat-{MAX_TESTED}.json").write_text(
            json.dumps({"obs": MAX_TESTED, **max_tested_result}), encoding="utf-8")
        return artifact_dir

    def test_max_tested_failing_at_obs_build_is_not_exit_ok(self):
        # Reproduces the bug report: 32.2.0 (top of the grid) green, 32.2.2
        # (max_tested, required) red at obs-build. The old code exited 0.
        # The SDK could not build on the runner, so the range cannot be
        # declared -- report this as EXIT_NO_RANGE, not EXIT_INCOMPATIBLE,
        # to direct the reader to CI setup, not the plugin code.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            artifact_dir = self._artifact_dir(root, unbuildable())
            import io
            stderr = io.StringIO()
            with mock.patch("sys.stderr", stderr):
                exit_code = obs_compat._report(artifact_dir, GRID, MAX_TESTED, None, root=root)
            self.assertEqual(exit_code, obs_compat.EXIT_NO_RANGE)
            stderr_text = stderr.getvalue()
            self.assertIn(MAX_TESTED, stderr_text)
            self.assertIn("could not be built", stderr_text)

    def test_max_tested_failing_at_plugin_build_exits_incompatible(self):
        # When the plugin itself fails against max_tested, that is a known,
        # specific incompatibility, not an undecidable range. _report must
        # return EXIT_INCOMPATIBLE with a message naming the plugin as the
        # cause, sending whoever reads it to the plugin code, not to CI setup.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            artifact_dir = self._artifact_dir(root, incompatible())
            import io
            stderr = io.StringIO()
            with mock.patch("sys.stderr", stderr):
                exit_code = obs_compat._report(artifact_dir, GRID, MAX_TESTED, None, root=root)
            self.assertEqual(exit_code, obs_compat.EXIT_INCOMPATIBLE)
            stderr_text = stderr.getvalue()
            self.assertIn(MAX_TESTED, stderr_text)
            self.assertIn("plugin", stderr_text.lower())


class ReportExcludesTheBetaFromTheGate(unittest.TestCase):
    """CRITICAL 2: a beta is probed for forward-looking information only. It
    is never in the declared range, never `required`, and the workflow
    already lets its own job fail without failing the run (continue-on-error
    is keyed off `required`). --report must not fail the whole run on a
    beta's behalf -- but its failure must still show up in the manifest and
    the summary table, since that is the entire point of probing it.
    """

    BETA = "32.3.0-beta1"

    def test_a_beta_broken_at_plugin_build_does_not_block_the_release(self):
        # CRITICAL 1: this pins the fix, not the defect it replaced. The old
        # version of this test left the README empty between the markers --
        # guaranteed stale no matter what --report computed -- then asserted
        # only that the exit code was not EXIT_INCOMPATIBLE. That passed
        # even though the run still returned EXIT_STALE and the release was
        # still skipped: the test's name claimed more than its assertions
        # checked. Here the README is seeded with exactly what a red,
        # unadvertised beta renders to (render_readme_section omits the beta
        # row unless its own result is green), so nothing is stale and
        # EXIT_OK -- not merely "not EXIT_INCOMPATIBLE" -- is the only way
        # this test can pass.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            write_buildspec(root)

            # What a red beta renders to: a failing row in the probe table, and
            # no "Also builds against" claim anywhere. The expectation carries
            # the red beta too, because the README the run produces will show
            # it -- suppressing the claim is not the same as hiding the probe.
            expected_results = dict(grid_results())
            expected_results[self.BETA] = incompatible()
            expected_manifest = obs_compat.build_manifest(
                expected_results, GRID, MAX_TESTED, self.BETA, "2026-08-31")
            (root / "README.md").write_text(readme_text(expected_manifest), encoding="utf-8", newline="\n")

            artifact_dir = root / "compat-artifacts"
            artifact_dir.mkdir()
            for version in GRID + [MAX_TESTED]:
                folder = artifact_dir / f"compat-{version}"
                folder.mkdir()
                (folder / f"compat-{version}.json").write_text(
                    json.dumps({"obs": version, **ok()}), encoding="utf-8")
            beta_folder = artifact_dir / f"compat-{self.BETA}"
            beta_folder.mkdir()
            (beta_folder / f"compat-{self.BETA}.json").write_text(
                json.dumps({"obs": self.BETA, **incompatible()}), encoding="utf-8")

            summary_file = root / "summary.txt"
            with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary_file)}):
                import io
                stderr = io.StringIO()
                with mock.patch("sys.stderr", stderr):
                    exit_code = obs_compat._report(
                        artifact_dir, GRID, MAX_TESTED, self.BETA, root=root)

            # The literal claim in this test's name: a red beta does not
            # block the release. EXIT_OK, not just "not EXIT_INCOMPATIBLE".
            self.assertEqual(exit_code, obs_compat.EXIT_OK)
            self.assertNotIn("does not build against", stderr.getvalue())

            # But the beta's failure is still recorded, not silently
            # dropped -- needs_full_run compares beta_tested against the
            # newly discovered beta on every daily watch, and dropping a red
            # result here would make that comparison re-fire the full matrix
            # every single day forever.
            written = obs_compat.load_manifest(root / "obs-compat.json")
            self.assertEqual(written["results"][self.BETA]["phase"], "plugin-build")
            self.assertEqual(written["beta_tested"], self.BETA)
            self.assertIn(self.BETA, summary_file.read_text(encoding="utf-8"))

    def test_the_readme_does_not_advertise_a_red_beta(self):
        # The other half of CRITICAL 1: even when the README genuinely needs
        # rewriting for an unrelated reason (here: it is empty between the
        # markers), the freshly-written manifest must never let a red beta's
        # mere presence render an "Also builds against" claim the same
        # manifest's own results contradict.
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            write_buildspec(root)
            (root / "README.md").write_text(readme_text(), encoding="utf-8", newline="\n")

            artifact_dir = root / "compat-artifacts"
            artifact_dir.mkdir()
            for version in GRID + [MAX_TESTED]:
                folder = artifact_dir / f"compat-{version}"
                folder.mkdir()
                (folder / f"compat-{version}.json").write_text(
                    json.dumps({"obs": version, **ok()}), encoding="utf-8")
            beta_folder = artifact_dir / f"compat-{self.BETA}"
            beta_folder.mkdir()
            (beta_folder / f"compat-{self.BETA}.json").write_text(
                json.dumps({"obs": self.BETA, **incompatible()}), encoding="utf-8")

            with mock.patch.dict(os.environ, {}, clear=False):
                os.environ.pop("GITHUB_STEP_SUMMARY", None)
                import io
                with mock.patch("sys.stdout", io.StringIO()), \
                     mock.patch("sys.stderr", io.StringIO()):
                    obs_compat._report(artifact_dir, GRID, MAX_TESTED, self.BETA, root=root)

            written = obs_compat.load_manifest(root / "obs-compat.json")
            rendered = obs_compat.render_readme_section(written)
            # The claim is suppressed; the measured failure is not hidden.
            self.assertNotIn("Also builds against", rendered)
            beta_row = next(line for line in rendered.splitlines()
                            if f"`{self.BETA}`" in line)
            self.assertIn("does not compile", beta_row)


class ReportExitsCleanWhenEverythingAlreadyMatches(unittest.TestCase):
    """I4: every other _report test in this file asserts 2, 3, 1 or 4 --
    the gate is proven to block and never proven to pass. A regression
    making _report always return non-zero would keep every one of those
    tests green while making every release silently unpublishable.
    """

    def test_a_fully_green_run_that_already_matches_the_readme_exits_ok(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            write_buildspec(root)

            manifest = obs_compat.build_manifest(
                grid_results(), GRID, MAX_TESTED, None, "2026-08-31")
            (root / "README.md").write_text(readme_text(manifest), encoding="utf-8", newline="\n")

            artifact_dir = root / "compat-artifacts"
            artifact_dir.mkdir()
            for version in GRID + [MAX_TESTED]:
                folder = artifact_dir / f"compat-{version}"
                folder.mkdir()
                (folder / f"compat-{version}.json").write_text(
                    json.dumps({"obs": version, **ok()}), encoding="utf-8")

            summary_file = root / "summary.txt"
            with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary_file)}):
                import io
                stderr = io.StringIO()
                with mock.patch("sys.stderr", stderr):
                    exit_code = obs_compat._report(
                        artifact_dir, GRID, MAX_TESTED, None, root=root)

            self.assertEqual(exit_code, obs_compat.EXIT_OK)
            # Nothing to report: no ::error::, no ::notice::, no complaint.
            self.assertEqual(stderr.getvalue(), "")


class ReportRequiresItsArguments(unittest.TestCase):
    # Exit 1 is reserved for a genuine incompatibility; malformed CLI input
    # must never fall through argparse/json.loads into Python's default
    # exit-status-1 crash and be mistaken for one.
    def test_report_without_grid_is_bad_input_not_exit_1(self):
        argv = ["obs_compat.py", "--report", "--latest-stable", "32.2.2"]
        with mock.patch.object(sys, "argv", argv):
            self.assertEqual(obs_compat.main(), obs_compat.EXIT_BAD_INPUT)

    def test_report_without_latest_stable_is_bad_input(self):
        argv = ["obs_compat.py", "--report", "--grid", "[]"]
        with mock.patch.object(sys, "argv", argv):
            self.assertEqual(obs_compat.main(), obs_compat.EXIT_BAD_INPUT)

    def test_report_with_unparseable_grid_is_bad_input(self):
        argv = ["obs_compat.py", "--report", "--grid", "not-json",
                "--latest-stable", "32.2.2"]
        with mock.patch.object(sys, "argv", argv):
            self.assertEqual(obs_compat.main(), obs_compat.EXIT_BAD_INPUT)


if __name__ == "__main__":
    unittest.main()
