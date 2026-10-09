import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import compatibility_eval
import compatibility_matrix


class CompatibilityEvaluationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def install(self, executable="game.exe", registry=True, libraries=True):
        game = self.root / "private-title-name"
        game.mkdir()
        (game / executable).write_bytes(b"MZ")
        if registry:
            (game / Path(executable).with_suffix(".registry.json")).write_text("[]")
        if libraries:
            (game / "libs").mkdir()
        return game

    def test_game_identity_does_not_expose_directory_name(self):
        game = self.install()
        identity = compatibility_eval.anonymous_game_id(game)
        self.assertRegex(identity, r"^game-[0-9a-f]{12}$")
        self.assertNotIn(game.name, identity)

    def test_find_game_accepts_directory_and_file(self):
        game = self.install()
        executable, root = compatibility_eval.find_game(game)
        self.assertEqual((executable, root), ((game / "game.exe").resolve(), game.resolve()))
        self.assertEqual(compatibility_eval.find_game(executable), (executable, root))

    def test_find_game_rejects_missing_and_ambiguous_executables(self):
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaisesRegex(compatibility_eval.EvaluationError, "no game.exe or game.elf"):
            compatibility_eval.find_game(empty)
        game = self.install()
        (game / "game.elf").write_bytes(b"\x7fELF")
        with self.assertRaisesRegex(compatibility_eval.EvaluationError, "both game.exe and game.elf"):
            compatibility_eval.find_game(game)

    def test_sanitize_removes_absolute_paths(self):
        game = self.install()
        value = compatibility_eval.sanitize(f"failed at {game / 'app0' / 'asset.bin'} and C:\\private\\dump.bin", (game,))
        self.assertNotIn(str(game), value)
        self.assertNotIn("C:\\private", value)
        self.assertIn("<GAME_DIR>", value)

    def test_static_evaluation_reports_missing_inputs(self):
        game = self.install(registry=False, libraries=False)
        result, imports = compatibility_eval.static_evaluation(game / "game.exe", game, self.root)
        self.assertEqual(imports, None)
        self.assertEqual(result["blockers"], ["registry_missing", "runtime_libraries_missing"])

    def test_installed_guest_module_keeps_original_library_alias(self):
        game = self.install()
        modules = game / "app0" / "sce_module"
        modules.mkdir(parents=True)
        (modules / "libGuest.prx.guest.prx").write_bytes(b"MZ")
        names = compatibility_eval.installed_module_names([modules])
        self.assertEqual(names, {"libGuest.prx.guest.prx", "libGuest.prx"})

    def test_guest_modules_outside_standard_directories_are_discovered(self):
        game = self.install()
        modules = game / "app0" / "engine" / "managed"
        modules.mkdir(parents=True)
        (modules / "GameRuntime.prx.guest.prx").write_bytes(b"MZ")
        self.assertIn(modules, compatibility_eval.find_module_directories(game))

    def test_static_evaluation_preserves_audit_accounting(self):
        game = self.install()
        summary = {
            "references": 4,
            "unique_imports": 3,
            "unique_by_class": {"implemented": 1, "stub": 1, "absent": 1, "module": 0},
            "references_by_class": {"implemented": 2, "stub": 1, "absent": 1, "module": 0},
            "missing_libraries": {"libMissing.prx": 1},
            "library_mismatch": 1,
        }
        records = [
            {"nid": "AAAAAAAAAAA", "library": "libA.prx", "references": 1, "class": "stub", "providers": ["libA.prx"], "library_mismatch": False, "name": "sceA"},
            {"nid": "BBBBBBBBBBB", "library": "libMissing.prx", "references": 1, "class": "absent", "providers": [], "library_mismatch": False, "name": None},
            {"nid": "CCCCCCCCCCC", "library": "libC.prx", "references": 2, "class": "implemented", "providers": ["libOther.prx"], "library_mismatch": True, "name": None},
        ]
        with patch.object(compatibility_eval.import_audit, "read_registry", return_value=[("AAAAAAAAAAA", "libA.prx")]), \
             patch.object(compatibility_eval.import_audit, "built_libraries", return_value=({}, {"libA.prx"})), \
             patch.object(compatibility_eval.import_audit, "module_files", return_value=set()), \
             patch.object(compatibility_eval.import_audit, "source_stubs", return_value={}), \
             patch.object(compatibility_eval.import_audit, "audit", return_value=(records, summary["missing_libraries"])), \
             patch.object(compatibility_eval.import_audit, "summarize", return_value=summary):
            result, problems = compatibility_eval.static_evaluation(game / "game.exe", game, self.root)
        self.assertEqual(result["blockers"], ["absent_imports", "missing_libraries", "windows_library_mismatch"])
        self.assertEqual(result["risks"], ["throwing_stubs"])
        self.assertEqual(len(problems), 3)

    def test_process_status_classifies_windows_and_signal_failures(self):
        self.assertEqual(compatibility_eval.process_status(-1073741819)["kind"], "access_violation")
        self.assertEqual(compatibility_eval.process_status(-9)["kind"], "signal")
        self.assertEqual(compatibility_eval.process_status(0)["kind"], "exit")

    def test_output_classification_is_structured(self):
        kinds = {item["kind"] for item in compatibility_eval.classify_output("Not implemented\nVK_ERROR_DEVICE_LOST\n")}
        self.assertEqual(kinds, {"runtime_stub", "graphics_failure"})

    def test_launch_probe_bounds_and_terminates_a_real_process(self):
        result = compatibility_eval.launch_probe(Path(sys.executable), self.root, 0.1)
        self.assertTrue(result["survived_observation"])
        self.assertEqual(result["exit"], None)
        self.assertGreaterEqual(result["duration_seconds"], 0.1)

    def test_ctest_summary_is_parsed_and_redacted(self):
        build = self.root / "build"
        build.mkdir()
        (build / "CTestTestfile.cmake").write_text("")
        output = b"100% tests passed, 0 tests failed out of 12\n"
        completed = subprocess.CompletedProcess([], 0, output)
        with patch.object(compatibility_eval.subprocess, "run", return_value=completed):
            result = compatibility_eval.run_ctest(build, 10)
        self.assertTrue(result["passed"])
        self.assertEqual((result["pass_percent"], result["failed"], result["total"]), (100, 0, 12))

    def test_main_writes_machine_readable_report_without_game_name(self):
        game = self.install(registry=False)
        report = self.root / "report.json"
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = compatibility_eval.main([str(game), "--json", str(report)])
        self.assertEqual((code, err.getvalue()), (1, ""))
        data = json.loads(report.read_text())
        self.assertEqual(data["verdict"], "load_blocked")
        self.assertNotIn(game.name, report.read_text())


class CompatibilityMatrixTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def test_discovery_is_recursive_and_case_insensitive(self):
        first = self.root / "one" / "EBOOT.BIN"
        second = self.root / "two" / "eboot.elf"
        ignored = self.root / "three" / "other.bin"
        for path in (first, second, ignored):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"image")
        found = compatibility_matrix.discover_sources(self.root)
        self.assertEqual(set(found), {first.resolve(), second.resolve()})

    def test_conversion_command_selects_target_and_intel_mode(self):
        command = compatibility_matrix.conversion_command(Path("relinker"), Path("input.bin"), Path("game.exe"), "windows", True)
        self.assertEqual(command, ["relinker", "--windows", "--windows-gui", "--to-intel", "--registry", "input.bin", "game.exe"])

    def test_aggregate_ranks_imports_by_affected_games(self):
        common = {"class": "stub", "library": "libA.prx", "nid": "AAAAAAAAAAA", "name": "sceA", "references": 1}
        other = {"class": "absent", "library": "libB.prx", "nid": "BBBBBBBBBBB", "name": None, "references": 2}
        values = compatibility_matrix.aggregate([
            {"problem_imports": [common, other]},
            {"problem_imports": [common]},
        ])
        self.assertEqual((values[0]["name"], values[0]["games"], values[0]["references"]), ("sceA", 2, 2))
        self.assertEqual(values[1]["games"], 1)


if __name__ == "__main__":
    unittest.main()
