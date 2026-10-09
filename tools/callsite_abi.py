import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

import compatibility_eval


class AnalysisError(RuntimeError):
    pass


class PeImage:
    def __init__(self, path):
        self.path = Path(path)
        try:
            self.data = self.path.read_bytes()
        except OSError as error:
            raise AnalysisError("executable cannot be read") from error
        if len(self.data) < 0x40 or self.data[:2] != b"MZ":
            raise AnalysisError("executable is not a PE image")
        pe = self._u32(0x3C)
        if pe + 24 > len(self.data) or self.data[pe:pe + 4] != b"PE\0\0":
            raise AnalysisError("executable has an invalid PE header")
        count = self._u16(pe + 6)
        optional_size = self._u16(pe + 20)
        optional = pe + 24
        if optional + optional_size > len(self.data) or self._u16(optional) != 0x20B:
            raise AnalysisError("executable is not PE32+")
        self.image_base = self._u64(optional + 24)
        table = optional + optional_size
        self.sections = []
        for index in range(count):
            offset = table + index * 40
            if offset + 40 > len(self.data):
                raise AnalysisError("executable section table is truncated")
            name = self.data[offset:offset + 8].split(b"\0", 1)[0].decode("ascii", errors="replace")
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", self.data, offset + 8)
            characteristics = self._u32(offset + 36)
            self.sections.append({
                "name": name,
                "rva": rva,
                "size": max(virtual_size, raw_size),
                "raw_size": raw_size,
                "raw_offset": raw_offset,
                "executable": bool(characteristics & 0x20000000),
            })

    def _u16(self, offset):
        if offset + 2 > len(self.data):
            raise AnalysisError("executable header is truncated")
        return struct.unpack_from("<H", self.data, offset)[0]

    def _u32(self, offset):
        if offset + 4 > len(self.data):
            raise AnalysisError("executable header is truncated")
        return struct.unpack_from("<I", self.data, offset)[0]

    def _u64(self, offset):
        if offset + 8 > len(self.data):
            raise AnalysisError("executable header is truncated")
        return struct.unpack_from("<Q", self.data, offset)[0]

    def location(self, rva):
        for section in self.sections:
            relative = rva - section["rva"]
            if 0 <= relative < section["raw_size"]:
                return section, section["raw_offset"] + relative
        return None, None


class ElfImage:
    def __init__(self, path):
        self.path = Path(path)
        try:
            self.data = self.path.read_bytes()
        except OSError as error:
            raise AnalysisError("executable cannot be read") from error
        if len(self.data) < 64 or self.data[:6] != b"\x7fELF\x02\x01":
            raise AnalysisError("executable is not a little-endian ELF64 image")
        program_offset = struct.unpack_from("<Q", self.data, 32)[0]
        entry_size, count = struct.unpack_from("<HH", self.data, 54)
        if entry_size < 56 or program_offset + entry_size * count > len(self.data):
            raise AnalysisError("executable program-header table is truncated")
        self.sections = []
        for index in range(count):
            offset = program_offset + index * entry_size
            kind, flags, raw_offset, virtual_address, _, raw_size, memory_size, _ = struct.unpack_from("<IIQQQQQQ", self.data, offset)
            if kind != 1:
                continue
            self.sections.append({
                "name": f"LOAD{index}",
                "rva": virtual_address,
                "size": memory_size,
                "raw_size": raw_size,
                "raw_offset": raw_offset,
                "executable": bool(flags & 1),
            })

    def location(self, address):
        for section in self.sections:
            relative = address - section["rva"]
            if 0 <= relative < section["raw_size"]:
                return section, section["raw_offset"] + relative
        return None, None


def load_image(path):
    try:
        with Path(path).open("rb") as stream:
            magic = stream.read(4)
    except OSError as error:
        raise AnalysisError("executable cannot be read") from error
    if magic[:2] == b"MZ":
        return PeImage(path)
    if magic == b"\x7fELF":
        return ElfImage(path)
    raise AnalysisError("executable is neither PE32+ nor ELF64")


def parse_hex(value, field):
    if not isinstance(value, str):
        raise AnalysisError(f"registry {field} is not a string")
    try:
        return int(value, 16)
    except ValueError as error:
        raise AnalysisError(f"registry {field} is not hexadecimal") from error


def load_registry(path):
    try:
        entries = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AnalysisError("registry cannot be read") from error
    if not isinstance(entries, list):
        raise AnalysisError("registry must contain a JSON list")
    values = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise AnalysisError(f"registry entry {index} is not an object")
        nid = entry.get("nid")
        library = entry.get("library")
        sites = entry.get("callSites")
        if not isinstance(nid, str) or not isinstance(library, str) or not isinstance(sites, list):
            raise AnalysisError(f"registry entry {index} has invalid identity or call sites")
        values.append({
            "nid": nid.split("#", 1)[0],
            "library": library,
            "target": parse_hex(entry.get("targetOffset"), "targetOffset"),
            "sites": [parse_hex(site, "callSites") for site in sites],
            "resolved": entry.get("callSitesResolved") is True,
        })
    return values


def decode_reference(image, site, target, width=8):
    section, offset = image.location(site)
    if section is None or not section["executable"]:
        return {"site": site, "status": "not_executable"}
    start = offset
    cursor = start
    rex = None
    if cursor < len(image.data) and 0x40 <= image.data[cursor] <= 0x4F:
        rex = image.data[cursor]
        cursor += 1
    if cursor + 6 > len(image.data):
        return {"site": site, "status": "truncated", "section": section["name"]}
    opcode = image.data[cursor]
    modrm = image.data[cursor + 1]
    if opcode not in (0x8B, 0xFF) or (modrm & 0xC7) != 0x05:
        return {"site": site, "status": "instruction_mismatch", "section": section["name"]}
    if opcode == 0x8B and (rex is None or not rex & 0x08):
        return {"site": site, "status": "instruction_mismatch", "section": section["name"]}
    length = cursor - start + 6
    displacement = struct.unpack_from("<i", image.data, cursor + 2)[0]
    actual = site + length + displacement
    group = (modrm >> 3) & 7
    if opcode == 0x8B:
        kind = "address_load"
    elif group == 2:
        kind = "direct_call"
    elif group == 4:
        kind = "tail_call"
    else:
        kind = "indirect_other"
    matches = target <= actual < target + width
    return {
        "site": site,
        "status": "verified" if matches else "target_mismatch",
        "kind": kind,
        "target": actual,
        "section": section["name"],
        "abi_evidence": "sysv_integer_registers" if matches and kind in ("direct_call", "tail_call") else None,
    }


ARGUMENT_ALIASES = {
    "rdi": {"rdi", "edi", "di", "dil"},
    "rsi": {"rsi", "esi", "si", "sil"},
    "rdx": {"rdx", "edx", "dx", "dl", "dh"},
    "rcx": {"rcx", "ecx", "cx", "cl", "ch"},
    "r8": {"r8", "r8d", "r8w", "r8b"},
    "r9": {"r9", "r9d", "r9w", "r9b"},
}
WRITING_MNEMONICS = re.compile(r"^(?:mov|movabs|movsx|movsxd|movzx|lea|xor|sub|add|and|or|imul|pop|cmov\w*|set\w*)$")
DISASSEMBLY_LINE = re.compile(r"^\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{2}\s+)+\s*([a-zA-Z][^#]*)")


def normalize_argument(operand):
    token = operand.strip().split(",", 1)[0].strip().lower()
    for canonical, aliases in ARGUMENT_ALIASES.items():
        if token in aliases:
            return canonical
    return None


def infer_argument_writes(disassembly, site, limit=16):
    instructions = []
    for line in disassembly.splitlines():
        match = DISASSEMBLY_LINE.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        assembly = match.group(2).strip()
        mnemonic, _, operands = assembly.partition(" ")
        instructions.append((address, mnemonic.lower(), operands.strip(), assembly))
    call_index = next((index for index, item in enumerate(instructions) if item[0] == site), None)
    if call_index is None:
        return {"status": "site_not_disassembled", "recent_argument_writes": []}
    writes = {}
    prior = instructions[max(0, call_index - limit):call_index]
    for distance, item in enumerate(reversed(prior), 1):
        address, mnemonic, operands, assembly = item
        if not WRITING_MNEMONICS.fullmatch(mnemonic):
            continue
        argument = normalize_argument(operands)
        if argument and argument not in writes:
            writes[argument] = {"register": argument, "instructions_before_call": distance, "address": address, "instruction": assembly}
    ordered = [writes[name] for name in ARGUMENT_ALIASES if name in writes]
    return {"status": "heuristic", "recent_argument_writes": ordered}


def disassemble_site(image, site, objdump, context_bytes=96):
    section, offset = image.location(site)
    if section is None:
        return {"status": "site_not_mapped", "recent_argument_writes": []}
    before = min(context_bytes, site - section["rva"], offset)
    after = 16
    snippet = image.data[offset - before:min(len(image.data), offset + after)]
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix="anyps5-abi-", suffix=".bin", delete=False) as stream:
            stream.write(snippet)
            temporary = stream.name
        command = [
            objdump,
            "-D",
            "-b", "binary",
            "-m", "i386:x86-64",
            "-M", "intel",
            f"--adjust-vma=0x{site - before:x}",
            temporary,
        ]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=15, check=False)
        if completed.returncode != 0:
            return {"status": "objdump_failed", "recent_argument_writes": []}
        return infer_argument_writes(completed.stdout, site)
    except (OSError, subprocess.TimeoutExpired):
        return {"status": "objdump_failed", "recent_argument_writes": []}
    finally:
        if temporary:
            try:
                os.unlink(temporary)
            except OSError:
                pass


def analyze_entries(image, entries, problems, objdump=None):
    problem_map = {(item["nid"], item["library"]): item for item in problems}
    rows = []
    for entry in entries:
        problem = problem_map.get((entry["nid"], entry["library"]))
        if problem is None:
            continue
        evidence = [decode_reference(image, site, entry["target"]) for site in entry["sites"]]
        statuses = Counter(item["status"] for item in evidence)
        kinds = Counter(item.get("kind") for item in evidence if item["status"] == "verified")
        first_site = min((item["site"] for item in evidence if item["status"] == "verified" and item.get("kind") in ("direct_call", "tail_call")), default=None)
        abi = disassemble_site(image, first_site, objdump) if objdump and first_site is not None else None
        rows.append({
            "nid": entry["nid"],
            "name": problem.get("name"),
            "library": entry["library"],
            "class": problem["class"],
            "registry_sites": len(entry["sites"]),
            "verified_sites": statuses["verified"],
            "direct_calls": kinds["direct_call"],
            "tail_calls": kinds["tail_call"],
            "address_loads": kinds["address_load"],
            "site_statuses": dict(sorted(statuses.items())),
            "first_verified_site": min((item["site"] for item in evidence if item["status"] == "verified"), default=None),
            "abi": abi,
            "confidence": "high" if statuses["verified"] else "none",
        })
    return sorted(rows, key=lambda item: (-item["direct_calls"], -item["tail_calls"], -item["verified_sites"], -item["registry_sites"], item["library"], item["nid"]))


def make_report(game, libraries, source, objdump=None):
    executable, root = compatibility_eval.find_game(game)
    static, problems = compatibility_eval.static_evaluation(executable, root, source, libraries)
    entries = load_registry(executable.with_suffix(".registry.json"))
    rows = analyze_entries(load_image(executable), entries, problems, objdump)
    invalid = any(row["verified_sites"] < row["registry_sites"] for row in rows)
    return {
        "schema": 1,
        "game": compatibility_eval.anonymous_game_id(root),
        "imports": static["imports"],
        "problem_imports": len(rows),
        "verified_problem_imports": sum(bool(row["verified_sites"]) for row in rows),
        "registry_warning": "Some registry sites do not match instructions in this executable; for Windows output, analyze a temporary Linux ELF conversion of the same dump before inferring ABIs." if invalid else None,
        "priorities": rows,
    }


def find_dump_executable(root):
    path = Path(root)
    if not path.is_dir():
        raise AnalysisError("dump path is not a directory")
    entries = sorted(item for item in path.rglob("*") if item.is_file() and item.name.lower() in ("eboot.bin", "eboot.elf"))
    if not entries:
        raise AnalysisError("dump contains no eboot.bin or eboot.elf")
    if len(entries) > 1:
        raise AnalysisError("dump contains multiple entry executables")
    return entries[0]


def analyze_dump(dump, relinker, libraries, source, objdump=None, timeout=600):
    entry = find_dump_executable(dump)
    executable = Path(relinker)
    if not executable.is_file():
        raise AnalysisError("relinker executable is missing")
    with tempfile.TemporaryDirectory(prefix="anyps5-abi-elf-") as directory:
        output = Path(directory) / "game.elf"
        try:
            completed = subprocess.run(
                [str(executable), "--registry", str(entry), str(output)],
                cwd=entry.parent,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise AnalysisError("temporary ELF conversion failed or timed out") from error
        if completed.returncode != 0:
            raise AnalysisError(f"temporary ELF conversion failed with status {completed.returncode}")
        report = make_report(output, libraries, source, objdump)
        report["game"] = compatibility_eval.anonymous_game_id(entry.parent)
        report["analysis_image"] = "temporary_linux_elf"
        return report


def render(report, limit):
    lines = [f"Call-site ABI analysis: {report['game']}", f"Problem imports: {report['problem_imports']}; byte-verified: {report['verified_problem_imports']}"]
    if report["registry_warning"]:
        lines.append(f"WARNING: {report['registry_warning']}")
    lines.append("Priority candidates:")
    for row in report["priorities"][:limit]:
        label = row["name"] or row["nid"]
        lines.append(f"  {row['confidence']:<4} {row['class']:<6} {row['library'] or '<unspecified>'} {label} verified={row['verified_sites']} recorded={row['registry_sites']}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="rank missing imports and verify registry call-site ABI evidence")
    parser.add_argument("game", type=Path, help="installed game directory or converted executable")
    parser.add_argument("--libs", type=Path, required=True, help="runtime PRX directory")
    parser.add_argument("--source", type=Path, default=compatibility_eval.DEFAULT_SOURCE)
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--infer-abi", action="store_true", help="use objdump to collect heuristic argument-register writes")
    parser.add_argument("--relinker", type=Path, help="treat game as a clean dump and analyze an automatically deleted Linux ELF conversion")
    parser.add_argument("--conversion-timeout", type=float, default=600)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.limit < 1:
            raise AnalysisError("limit must be positive")
        objdump = shutil.which("objdump") if args.infer_abi else None
        if args.infer_abi and objdump is None:
            raise AnalysisError("objdump is required for --infer-abi")
        report = analyze_dump(args.game, args.relinker, args.libs, args.source, objdump, args.conversion_timeout) if args.relinker else make_report(args.game, args.libs, args.source, objdump)
        if args.json:
            args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (AnalysisError, compatibility_eval.EvaluationError, OSError) as error:
        print(f"FAIL: {compatibility_eval.sanitize(str(error))}", file=sys.stderr)
        return 2
    print(render(report, args.limit))
    return 1 if report["registry_warning"] else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    raise SystemExit(main())
