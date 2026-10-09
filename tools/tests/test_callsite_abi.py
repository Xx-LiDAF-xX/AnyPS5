import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import callsite_abi


def pe_image(path):
    data = bytearray(0x600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 1, 0, 0, 0, 0xF0, 0x22)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    section = optional + 0xF0
    data[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x400, 0x1000, 0x400, 0x200)
    struct.pack_into("<I", data, section + 36, 0x60000020)
    site = 0x1010
    target = 0x1100
    data[0x210:0x216] = b"\xFF\x15" + struct.pack("<i", target - (site + 6))
    path.write_bytes(data)
    return site, target


def elf_image(path):
    data = bytearray(0x600)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<HHI", data, 16, 2, 0x3E, 1)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HH", data, 54, 56, 1)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0x200, 0x1000, 0, 0x400, 0x400, 0x1000)
    site = 0x1010
    target = 0x1100
    data[0x210:0x216] = b"\xFF\x15" + struct.pack("<i", target - (site + 6))
    path.write_bytes(data)
    return site, target


class CallsiteAbiTests(unittest.TestCase):
    def test_verified_direct_call(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.exe"
            site, target = pe_image(path)
            result = callsite_abi.decode_reference(callsite_abi.PeImage(path), site, target)
            self.assertEqual(result["status"], "verified")
            self.assertEqual(result["kind"], "direct_call")
            self.assertEqual(result["abi_evidence"], "sysv_integer_registers")

    def test_rejects_stale_site(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.exe"
            _, target = pe_image(path)
            result = callsite_abi.decode_reference(callsite_abi.PeImage(path), 0x1020, target)
            self.assertEqual(result["status"], "instruction_mismatch")

    def test_verified_elf_direct_call(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.elf"
            site, target = elf_image(path)
            result = callsite_abi.decode_reference(callsite_abi.load_image(path), site, target)
            self.assertEqual(result["status"], "verified")
            self.assertEqual(result["kind"], "direct_call")

    def test_detects_target_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.exe"
            site, target = pe_image(path)
            result = callsite_abi.decode_reference(callsite_abi.PeImage(path), site, target + 8)
            self.assertEqual(result["status"], "target_mismatch")

    def test_registry_strips_symbol_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.registry.json"
            path.write_text(json.dumps([{
                "nid": "abcdefghijk#A#B",
                "library": "libExample.prx",
                "targetOffset": "0x1100",
                "callSites": ["0x1010"],
                "callSitesResolved": True,
            }]), encoding="utf-8")
            entry = callsite_abi.load_registry(path)[0]
            self.assertEqual(entry["nid"], "abcdefghijk")
            self.assertEqual(entry["sites"], [0x1010])

    def test_priority_prefers_verified_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.exe"
            site, target = pe_image(path)
            image = callsite_abi.PeImage(path)
            entries = [
                {"nid": "missing-one", "library": "a.prx", "target": target, "sites": [0x1020], "resolved": True},
                {"nid": "missing-two", "library": "b.prx", "target": target, "sites": [site], "resolved": True},
            ]
            problems = [
                {"nid": "missing-one", "library": "a.prx", "class": "absent", "name": None},
                {"nid": "missing-two", "library": "b.prx", "class": "absent", "name": None},
            ]
            rows = callsite_abi.analyze_entries(image, entries, problems)
            self.assertEqual(rows[0]["nid"], "missing-two")
            self.assertEqual(rows[0]["confidence"], "high")
            self.assertEqual(rows[1]["confidence"], "none")

    def test_argument_write_inference(self):
        disassembly = """
  1010: 8b 7d 90              mov    edi,DWORD PTR [rbp-0x70]
  1013: 4c 89 8d 68 ff ff ff  mov    QWORD PTR [rbp-0x98],r9
  101a: ff 15 e0 00 00 00     call   QWORD PTR [rip+0xe0]
"""
        result = callsite_abi.infer_argument_writes(disassembly, 0x101A)
        self.assertEqual(result["status"], "heuristic")
        self.assertEqual([item["register"] for item in result["recent_argument_writes"]], ["rdi"])

    def test_dump_entry_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested = root / "image0"
            nested.mkdir()
            entry = nested / "eboot.bin"
            entry.write_bytes(b"plain test fixture")
            self.assertEqual(callsite_abi.find_dump_executable(root), entry)

    def test_dump_entry_rejects_ambiguity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "eboot.bin").write_bytes(b"first")
            (root / "eboot.elf").write_bytes(b"second")
            with self.assertRaises(callsite_abi.AnalysisError):
                callsite_abi.find_dump_executable(root)


if __name__ == "__main__":
    unittest.main()
