import argparse
import json
import os
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

import compatibility_eval


def discover_sources(root):
    path = Path(root)
    if not path.is_dir():
        raise compatibility_eval.EvaluationError("dump root is not a directory")
    return sorted(
        (item.resolve() for item in path.rglob("*") if item.is_file() and item.name.lower() in ("eboot.bin", "eboot.elf")),
        key=lambda item: os.path.normcase(str(item)),
    )


def conversion_command(relinker, source, output, target, to_intel):
    command = [str(relinker)]
    if target == "windows":
        command.extend(("--windows", "--windows-gui"))
    if to_intel:
        command.append("--to-intel")
    command.extend(("--registry", str(source), str(output)))
    return command


def evaluate_dump(source, relinker, libraries, runtime_source, target, to_intel, timeout):
    identity = compatibility_eval.anonymous_game_id(source.parent)
    with tempfile.TemporaryDirectory(prefix="anyps5-compat-") as directory:
        root = Path(directory)
        output = root / ("game.exe" if target == "windows" else "game.elf")
        try:
            completed = subprocess.run(
                conversion_command(relinker, source, output, target, to_intel),
                cwd=source.parent,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
            raw = completed.stdout.decode("utf-8", errors="replace")
        except subprocess.TimeoutExpired as error:
            raw = (error.stdout or b"").decode("utf-8", errors="replace")
            return {
                "game": identity,
                "conversion": "timed_out",
                "signals": compatibility_eval.classify_output(raw),
                "output_excerpt": compatibility_eval.sanitize(raw, (source.parent, root)).splitlines()[-20:],
            }
        if completed.returncode != 0:
            clean = compatibility_eval.sanitize(raw, (source.parent, root))
            return {
                "game": identity,
                "conversion": "failed",
                "return_code": completed.returncode,
                "signals": compatibility_eval.classify_output(clean),
                "output_excerpt": [line for line in clean.splitlines()[-20:] if line.strip()],
            }
        static, problems = compatibility_eval.static_evaluation(output, root, runtime_source, libraries)
        return {
            "game": identity,
            "conversion": "passed",
            "verdict": compatibility_eval.verdict(static, None, None),
            "static": static,
            "problem_imports": problems,
        }


def aggregate(games):
    game_counts = Counter()
    references = Counter()
    details = {}
    for game in games:
        for record in game.get("problem_imports") or []:
            key = (record["class"], record["library"], record["nid"])
            game_counts[key] += 1
            references[key] += record["references"]
            details[key] = record.get("name")
    values = []
    for key, count in game_counts.items():
        kind, library, nid = key
        values.append({
            "class": kind,
            "library": library,
            "nid": nid,
            "name": details[key],
            "games": count,
            "references": references[key],
        })
    return sorted(values, key=lambda item: (-item["games"], item["class"], item["library"], item["nid"]))


def render(report):
    games = report["games"]
    passed = sum(game["conversion"] == "passed" for game in games)
    blocked = sum(game.get("verdict") == "load_blocked" for game in games)
    risks = sum(game.get("verdict") == "static_risk" for game in games)
    lines = [
        f"Compatibility matrix: {len(games)} games",
        f"Conversions: {passed} passed, {len(games) - passed} failed or timed out",
        f"Static verdicts: {blocked} load-blocked, {risks} with imported stubs",
    ]
    shared = report["shared_problem_imports"]
    if shared:
        lines.append("Highest-impact problem imports:")
        for item in shared[:20]:
            name = item["name"] or item["nid"]
            library = item["library"] or "<unspecified>"
            lines.append(f"  {item['games']} games  {item['class']:<6}  {library}  {name}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="build an anonymized static compatibility matrix for a dump collection")
    parser.add_argument("dump_root", type=Path)
    parser.add_argument("--relinker", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=compatibility_eval.DEFAULT_SOURCE)
    parser.add_argument("--target", choices=("windows", "linux"), default="windows" if os.name == "nt" else "linux")
    parser.add_argument("--to-intel", action="store_true")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    try:
        sources = discover_sources(args.dump_root)
        if not sources:
            raise compatibility_eval.EvaluationError("dump root contains no eboot.bin or eboot.elf")
        if not args.relinker.is_file():
            raise compatibility_eval.EvaluationError("relinker executable is missing")
        if not args.libs.is_dir():
            raise compatibility_eval.EvaluationError("runtime PRX directory is missing")
        games = []
        for index, source in enumerate(sources, 1):
            identity = compatibility_eval.anonymous_game_id(source.parent)
            print(f"Evaluating {index}/{len(sources)}: {identity}", flush=True)
            games.append(evaluate_dump(source, args.relinker, args.libs, args.source, args.target, args.to_intel, args.timeout))
        commit, dirty = compatibility_eval.import_audit.git_stamp()
        report = {
            "schema": 1,
            "source_commit": commit,
            "source_dirty": dirty,
            "target": args.target,
            "to_intel": args.to_intel,
            "games": games,
            "shared_problem_imports": aggregate(games),
        }
        if args.json:
            args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (compatibility_eval.EvaluationError, OSError) as error:
        print(f"FAIL: {compatibility_eval.sanitize(str(error))}", file=sys.stderr)
        return 2
    print(render(report))
    return 1 if any(game["conversion"] != "passed" or game.get("verdict") == "load_blocked" for game in games) else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    raise SystemExit(main())
