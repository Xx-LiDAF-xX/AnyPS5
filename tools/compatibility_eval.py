import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import import_audit


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = ROOT / "core" / "libs" / "prx"
WINDOWS_FAILURES = {
    0xC0000005: ("access_violation", "Guest or runtime code accessed invalid memory."),
    0xC000001D: ("illegal_instruction", "The host executed an unsupported CPU instruction."),
    0xC0000094: ("integer_divide_by_zero", "Guest or runtime code divided an integer by zero."),
    0xC00000FD: ("stack_overflow", "Guest or runtime code exhausted the process stack."),
    0xC0000135: ("missing_dependency", "Windows could not load a required library."),
    0xC0000139: ("missing_entry_point", "Windows could not resolve a required export."),
}
OUTPUT_SIGNALS = (
    ("runtime_stub", re.compile(r"not implemented|NotImplemented_nid", re.IGNORECASE), "A called runtime export is still a throwing stub."),
    ("missing_export", re.compile(r"undefined symbol|missing (?:export|symbol)|procedure could not be found", re.IGNORECASE), "A required export could not be resolved."),
    ("graphics_failure", re.compile(r"VK_ERROR|Vulkan[^\r\n]*(?:fail|error|unsupported)", re.IGNORECASE), "Vulkan initialization or execution reported a failure."),
    ("unhandled_syscall", re.compile(r"unsupported syscall|unhandled syscall", re.IGNORECASE), "Guest code reached an unsupported syscall."),
    ("runtime_failure", re.compile(r"(?:^|\s)FAIL:", re.IGNORECASE), "The runtime emitted an explicit failure."),
)
ABSOLUTE_PATH = re.compile(r"(?<![A-Za-z0-9_])(?:[A-Za-z]:[\\/]|/)(?:[^\s\"'<>|]+[\\/])*[^\s\"'<>|]*")


class EvaluationError(Exception):
    pass


def anonymous_game_id(path):
    value = os.path.normcase(str(path.resolve())).encode("utf-8", errors="surrogatepass")
    return "game-" + hashlib.sha256(value).hexdigest()[:12]


def sanitize(text, roots=()):
    value = text
    replacements = [(str(Path.home()), "<HOME>"), (str(ROOT), "<REPO>")]
    replacements.extend((str(Path(root).resolve()), "<GAME_DIR>") for root in roots)
    for source, replacement in sorted(replacements, key=lambda item: len(item[0]), reverse=True):
        if source:
            value = value.replace(source, replacement).replace(source.replace("\\", "/"), replacement)
    return ABSOLUTE_PATH.sub("<PATH>", value)


def find_game(value):
    selected = Path(value)
    if selected.is_file():
        return selected.resolve(), selected.resolve().parent
    if not selected.is_dir():
        raise EvaluationError("game path is not a file or directory")
    candidates = [selected / "game.exe", selected / "game.elf"]
    found = [path.resolve() for path in candidates if path.is_file()]
    if not found:
        raise EvaluationError("installed game directory has no game.exe or game.elf")
    if len(found) > 1:
        raise EvaluationError("installed game directory contains both game.exe and game.elf")
    return found[0], selected.resolve()


def find_module_directories(game_root):
    candidates = []
    for parent in (game_root, game_root / "app0"):
        for name in ("sce_module", "sce_modules", "prx"):
            candidate = parent / name
            if candidate.is_dir():
                candidates.append(candidate)
    app0 = game_root / "app0"
    if app0.is_dir():
        candidates.extend(item.parent for item in app0.rglob("*") if item.is_file() and item.name.lower().endswith(".guest.prx"))
    return sorted(set(candidates))


def installed_module_names(directories):
    names = import_audit.module_files(directories)
    aliases = set(names)
    suffix = ".guest.prx"
    for name in names:
        if name.lower().endswith(suffix):
            aliases.add(name[:-len(suffix)])
    return aliases


def static_evaluation(executable, game_root, source, libraries=None):
    registry = executable.with_suffix(".registry.json")
    libraries = Path(libraries) if libraries else game_root / "libs"
    result = {
        "executable_present": executable.is_file(),
        "registry_present": registry.is_file(),
        "libraries_present": libraries.is_dir(),
        "module_directories": len(find_module_directories(game_root)),
    }
    blockers = []
    if not registry.is_file():
        blockers.append("registry_missing")
    if not libraries.is_dir():
        blockers.append("runtime_libraries_missing")
    if blockers:
        result["blockers"] = blockers
        return result, None
    try:
        imports = import_audit.read_registry(registry)
        exports, library_names = import_audit.built_libraries([libraries])
        module_directories = find_module_directories(game_root)
        modules = installed_module_names(module_directories)
        records, missing = import_audit.audit(
            imports,
            exports,
            import_audit.source_stubs(source),
            library_names,
            modules,
        )
        summary = import_audit.summarize(records, len(imports), missing)
    except import_audit.AuditError as error:
        raise EvaluationError(sanitize(str(error), (game_root,))) from error
    result["imports"] = summary
    result["blockers"] = []
    if summary["unique_by_class"]["absent"]:
        result["blockers"].append("absent_imports")
    if summary["missing_libraries"]:
        result["blockers"].append("missing_libraries")
    if summary["library_mismatch"]:
        result["blockers"].append("windows_library_mismatch")
    result["risks"] = []
    if summary["unique_by_class"]["stub"]:
        result["risks"].append("throwing_stubs")
    compact = [
        {
            "nid": record["nid"],
            "library": record["library"],
            "class": record["class"],
            "references": record["references"],
            "name": record["name"],
            "library_mismatch": record["library_mismatch"],
        }
        for record in records
        if record["class"] in ("absent", "stub") or record["library_mismatch"]
    ]
    return result, compact


def classify_output(output):
    signals = []
    for name, pattern, explanation in OUTPUT_SIGNALS:
        if pattern.search(output):
            signals.append({"kind": name, "detail": explanation})
    return signals


def process_status(returncode):
    if returncode is None:
        return None
    native = returncode & 0xFFFFFFFF
    if native in WINDOWS_FAILURES:
        kind, detail = WINDOWS_FAILURES[native]
        return {"kind": kind, "detail": detail, "code": f"0x{native:08x}"}
    if returncode < 0:
        return {"kind": "signal", "detail": "The process was terminated by an operating-system signal.", "code": str(-returncode)}
    return {"kind": "exit", "detail": "The process exited during the observation window.", "code": str(returncode)}


def launch_probe(executable, game_root, seconds):
    started = time.monotonic()
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if os.name == "nt" else 0
    try:
        process = subprocess.Popen(
            [str(executable)],
            cwd=game_root,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
    except OSError as error:
        raise EvaluationError(f"cannot start installed game: operating-system error {error.errno}") from error
    survived = False
    try:
        output, _ = process.communicate(timeout=seconds)
    except subprocess.TimeoutExpired:
        survived = True
        process.terminate()
        try:
            output, _ = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()
    decoded = output.decode("utf-8", errors="replace") if output else ""
    sanitized = sanitize(decoded, (game_root,))
    lines = [line for line in sanitized.splitlines() if line.strip()]
    return {
        "duration_seconds": round(time.monotonic() - started, 3),
        "observation_seconds": seconds,
        "survived_observation": survived,
        "exit": None if survived else process_status(process.returncode),
        "signals": classify_output(sanitized),
        "output_excerpt": lines[-30:],
    }


def run_ctest(build, timeout):
    build_path = Path(build)
    if not (build_path / "CTestTestfile.cmake").is_file():
        raise EvaluationError("build directory has no CTestTestfile.cmake")
    try:
        completed = subprocess.run(
            ["ctest", "--test-dir", str(build_path), "--output-on-failure"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        timed_out = False
        output = completed.stdout.decode("utf-8", errors="replace")
        returncode = completed.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        output = (error.stdout or b"").decode("utf-8", errors="replace")
        returncode = None
    clean = sanitize(output, (build_path,))
    match = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", clean)
    return {
        "passed": bool(returncode == 0 and not timed_out),
        "timed_out": timed_out,
        "return_code": returncode,
        "pass_percent": int(match.group(1)) if match else None,
        "failed": int(match.group(2)) if match else None,
        "total": int(match.group(3)) if match else None,
        "output_excerpt": [line for line in clean.splitlines()[-30:] if line.strip()],
    }


def recommendations(static, probe, tests):
    values = []
    blockers = static.get("blockers", [])
    risks = static.get("risks", [])
    if "registry_missing" in blockers:
        values.append("Reinstall or relink with --registry so imports can be measured.")
    if "runtime_libraries_missing" in blockers:
        values.append("Install the built runtime libraries beside the game before launch testing.")
    if "absent_imports" in blockers or "missing_libraries" in blockers:
        values.append("Implement or supply the reported missing libraries and exports before debugging gameplay.")
    if "windows_library_mismatch" in blockers:
        values.append("Correct provider library names because Windows resolves imports per module.")
    if "throwing_stubs" in risks:
        values.append("Prioritize reported throwing stubs that appear in the title's import set.")
    if tests and not tests["passed"]:
        values.append("Fix the failing project tests before attributing a failure to the game.")
    if probe:
        kinds = {signal["kind"] for signal in probe["signals"]}
        if probe["exit"]:
            kinds.add(probe["exit"]["kind"])
        if "missing_dependency" in kinds or "missing_entry_point" in kinds or "missing_export" in kinds:
            values.append("Resolve loader dependencies and exports before investigating guest execution.")
        if "illegal_instruction" in kinds:
            values.append("Relink with Intel compatibility enabled or verify the host CPU feature set.")
        if "access_violation" in kinds:
            values.append("Capture a native stack trace and test an isolated experimental patch with recorded before/after behavior.")
        if "graphics_failure" in kinds:
            values.append("Record the selected Vulkan device, driver version and first failing Vulkan result.")
        if probe["survived_observation"]:
            values.append("Continue manual checks for rendering, input, audio, save data and scene transitions; startup survival is not proof of playability.")
    return values


def verdict(static, probe, tests):
    if tests and not tests["passed"]:
        return "project_tests_failed"
    if static.get("blockers"):
        return "load_blocked"
    if probe and not probe["survived_observation"]:
        return "startup_failed"
    if probe and probe["survived_observation"]:
        return "startup_survived"
    if static.get("risks"):
        return "static_risk"
    return "static_ready"


def render(report):
    lines = [f"Compatibility evaluation: {report['game']}", f"Verdict: {report['verdict']}"]
    static = report["static"]
    imports = static.get("imports")
    if imports:
        classes = imports["unique_by_class"]
        lines.append(
            f"Imports: {imports['unique_imports']} unique; {classes['implemented']} implemented, "
            f"{classes['module']} guest-module, {classes['stub']} stub, {classes['absent']} absent"
        )
    if report.get("tests"):
        tests = report["tests"]
        lines.append(f"Tests: {'passed' if tests['passed'] else 'failed'}" + (f" ({tests['total']} total)" if tests["total"] is not None else ""))
    if report.get("probe"):
        probe = report["probe"]
        lines.append(f"Launch probe: {'survived' if probe['survived_observation'] else 'exited'} after {probe['duration_seconds']:.3f}s")
        if probe["exit"]:
            lines.append(f"Exit: {probe['exit']['kind']} ({probe['exit']['code']})")
        if probe["signals"]:
            lines.append("Signals: " + ", ".join(signal["kind"] for signal in probe["signals"]))
    if report["recommendations"]:
        lines.append("Recommendations:")
        lines.extend(f"  - {value}" for value in report["recommendations"])
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="evaluate an installed game's AnyPS5 compatibility blockers")
    parser.add_argument("game", type=Path, help="installed game directory or game.exe/game.elf")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="runtime source tree used to identify throwing stubs")
    parser.add_argument("--libs", type=Path, help="runtime PRX directory; defaults to the installed game's libs directory")
    parser.add_argument("--ctest", type=Path, help="run CTest in this configured build directory")
    parser.add_argument("--test-timeout", type=float, default=600, help="maximum CTest seconds (default: 600)")
    parser.add_argument("--probe-seconds", type=float, default=0, help="launch and observe the game for this many seconds")
    parser.add_argument("--json", type=Path, help="write the structured report to this path")
    args = parser.parse_args(argv)
    if args.probe_seconds < 0 or args.test_timeout <= 0:
        parser.error("timeouts must be positive, and --probe-seconds may be zero")
    try:
        executable, game_root = find_game(args.game)
        static, imports = static_evaluation(executable, game_root, args.source, args.libs)
        tests = run_ctest(args.ctest, args.test_timeout) if args.ctest else None
        probe = launch_probe(executable, game_root, args.probe_seconds) if args.probe_seconds else None
        commit, dirty = import_audit.git_stamp()
        report = {
            "schema": 1,
            "game": anonymous_game_id(game_root),
            "source_commit": commit,
            "source_dirty": dirty,
            "static": static,
            "problem_imports": imports,
            "tests": tests,
            "probe": probe,
        }
        report["verdict"] = verdict(static, probe, tests)
        report["recommendations"] = recommendations(static, probe, tests)
        if args.json:
            args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (EvaluationError, OSError) as error:
        print(f"FAIL: {sanitize(str(error))}", file=sys.stderr)
        return 2
    print(render(report))
    return 1 if report["verdict"] in ("project_tests_failed", "load_blocked", "startup_failed") else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    raise SystemExit(main())
