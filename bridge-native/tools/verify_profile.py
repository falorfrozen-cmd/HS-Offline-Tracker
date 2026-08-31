#!/usr/bin/env python3
"""Static, read-only audit for the one reviewed HS Offline Tracker build profile."""

from __future__ import annotations

import hashlib
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


EXPECTED_SIZE = 303_584_768
EXPECTED_SHA256 = "5f8085456a27109681403d8c57533e6999fbd0664752ff5f2856985b5fbbde71"
EXPECTED_TIMESTAMP = 0x6A8C4540
EXPECTED_MACHINE = 0x8664
EXPECTED_SPANS = {
    "gml_Object_Loot_Ground_obj_Create_0": 0x1690,
    "gml_Script_anon@1084@gml_Object_Loot_Ground_obj_Create_0": 0x58B0,
    "gml_Script_GetRareDropAnnouncement": 0x0710,
}
GROUND = "gml_Object_Loot_Ground_obj_Create_0"
CALLER = "gml_Script_anon@1084@gml_Object_Loot_Ground_obj_Create_0"
ANNOUNCEMENT = "gml_Script_GetRareDropAnnouncement"


@dataclass(frozen=True)
class Section:
    name: str
    rva: int
    virtual_size: int
    raw_size: int
    raw_offset: int
    characteristics: int

    @property
    def readable(self) -> bool:
        return bool(self.characteristics & 0x40000000)

    @property
    def executable(self) -> bool:
        return bool(self.characteristics & 0x20000000)


class PeImage:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if len(data) < 0x100 or data[:2] != b"MZ":
            raise ValueError("invalid DOS header")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if pe + 24 > len(data) or data[pe : pe + 4] != b"PE\0\0":
            raise ValueError("invalid PE signature")
        self.machine, section_count, self.timestamp = struct.unpack_from("<HHI", data, pe + 4)
        optional_size = struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        if optional + optional_size > len(data) or struct.unpack_from("<H", data, optional)[0] != 0x20B:
            raise ValueError("not a PE32+ image")
        self.image_base = struct.unpack_from("<Q", data, optional + 24)[0]
        section_table = optional + optional_size
        self.sections: list[Section] = []
        for index in range(section_count):
            offset = section_table + index * 40
            if offset + 40 > len(data):
                raise ValueError("truncated section table")
            name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, offset + 8)
            characteristics = struct.unpack_from("<I", data, offset + 36)[0]
            if raw_size and raw_offset + raw_size > len(data):
                raise ValueError(f"section {name} exceeds file")
            self.sections.append(Section(name, rva, virtual_size, raw_size, raw_offset, characteristics))

    def section_for_rva(self, rva: int, size: int = 1) -> Section | None:
        if rva < 0 or size < 0:
            return None
        for section in self.sections:
            extent = max(section.virtual_size, section.raw_size)
            if section.rva <= rva and rva + size <= section.rva + extent:
                return section
        return None

    def rva_to_offset(self, rva: int, size: int = 1) -> int | None:
        section = self.section_for_rva(rva, size)
        if section is None:
            return None
        relative = rva - section.rva
        if relative + size > section.raw_size:
            return None
        return section.raw_offset + relative

    def va_to_rva(self, va: int) -> int | None:
        return va - self.image_base if va >= self.image_base else None

    def bytes_at_rva(self, rva: int, size: int) -> bytes:
        offset = self.rva_to_offset(rva, size)
        if offset is None:
            raise ValueError(f"unmapped RVA span (size={size})")
        return self.data[offset : offset + size]

    def c_string_at_va(self, va: int, maximum: int = 1024) -> str | None:
        rva = self.va_to_rva(va)
        if rva is None:
            return None
        offset = self.rva_to_offset(rva)
        if offset is None:
            return None
        end = self.data.find(b"\0", offset, min(len(self.data), offset + maximum + 1))
        if end < 0:
            return None
        raw = self.data[offset:end]
        if not raw.startswith(b"gml_") or any(byte < 0x20 or byte > 0x7E for byte in raw):
            return None
        return raw.decode("ascii")


def signed32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def direct_calls(code: bytes, code_rva: int, target_rva: int) -> list[int]:
    calls: list[int] = []
    for offset in range(0, len(code) - 4):
        if code[offset] == 0xE8 and code_rva + offset + 5 + signed32(code, offset + 1) == target_rva:
            calls.append(code_rva + offset)
    return calls


def rip_lea_references(code: bytes, code_rva: int, target_rva: int) -> list[int]:
    references: list[int] = []
    for offset in range(0, len(code) - 6):
        rex, opcode, modrm = code[offset : offset + 3]
        if (rex & 0xF8) != 0x48 or opcode != 0x8D or (modrm & 0xC7) != 0x05:
            continue
        if code_rva + offset + 7 + signed32(code, offset + 3) == target_rva:
            references.append(code_rva + offset)
    return references


def registration_rows(image: PeImage) -> dict[str, set[int]]:
    rows: dict[str, set[int]] = {}
    executable = [section for section in image.sections if section.executable]
    for section in image.sections:
        if not section.readable or section.executable or section.raw_size < 24:
            continue
        raw = image.data[section.raw_offset : section.raw_offset + section.raw_size]
        for row in range(0, len(raw) - 23, 8):
            name_va, code_va = struct.unpack_from("<QQ", raw, row + 8)
            code_rva = image.va_to_rva(code_va)
            if code_rva is None or not any(
                candidate.rva <= code_rva < candidate.rva + max(candidate.virtual_size, candidate.raw_size)
                for candidate in executable
            ):
                continue
            name = image.c_string_at_va(name_va)
            if name is not None:
                rows.setdefault(name, set()).add(code_rva)
    return rows


def audit(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    sha256 = hashlib.sha256(data).hexdigest()
    image = PeImage(data)
    checks: dict[str, bool] = {
        "exact_size": len(data) == EXPECTED_SIZE,
        "exact_sha256": sha256 == EXPECTED_SHA256,
        "exact_timestamp": image.timestamp == EXPECTED_TIMESTAMP,
        "exact_machine": image.machine == EXPECTED_MACHINE,
    }
    if not all(checks.values()):
        return {
            "ok": False,
            "path": str(path),
            "checks": checks,
            "actual": {
                "size": len(data),
                "sha256": sha256,
                "timestamp": f"0x{image.timestamp:08x}",
                "machine": f"0x{image.machine:04x}",
            },
        }

    rows = registration_rows(image)
    resolved: dict[str, int] = {}
    for name in EXPECTED_SPANS:
        matches = rows.get(name, set())
        checks[f"unique_name:{name}"] = len(matches) == 1
        if len(matches) == 1:
            resolved[name] = next(iter(matches))
    if len(resolved) != len(EXPECTED_SPANS):
        return {"ok": False, "path": str(path), "checks": checks}

    all_code = sorted({address for matches in rows.values() for address in matches})
    for name, expected_span in EXPECTED_SPANS.items():
        address = resolved[name]
        next_address = next((candidate for candidate in all_code if candidate > address), None)
        actual_span = None if next_address is None else next_address - address
        checks[f"registered_span:{name}"] = actual_span == expected_span

    ground = image.bytes_at_rva(resolved[GROUND], EXPECTED_SPANS[GROUND])
    caller = image.bytes_at_rva(resolved[CALLER], EXPECTED_SPANS[CALLER])
    announcement = image.bytes_at_rva(resolved[ANNOUNCEMENT], EXPECTED_SPANS[ANNOUNCEMENT])
    checks["ground_references_caller_once"] = len(
        rip_lea_references(ground, resolved[GROUND], resolved[CALLER])
    ) == 1

    calls = direct_calls(caller, resolved[CALLER], resolved[ANNOUNCEMENT])
    checks["caller_calls_announcement_once"] = len(calls) == 1
    all_calls: list[int] = []
    for section in image.sections:
        if not section.readable or not section.executable or not section.raw_size:
            continue
        code = image.data[section.raw_offset : section.raw_offset + section.raw_size]
        all_calls.extend(direct_calls(code, section.rva, resolved[ANNOUNCEMENT]))
    checks["announcement_has_one_direct_caller"] = len(all_calls) == 1

    if calls:
        call_offset = calls[0] - resolved[CALLER]
        contract = caller[max(0, call_offset - 64) : call_offset]
        checks["argument_count_is_three"] = b"\x41\xb9\x03\x00\x00\x00" in contract
        checks["argument_array_stack_contract"] = b"\x48\x89\x44\x24\x20" in contract
        checks["result_slot_contract"] = b"\x4c\x8d\x44\x24\x30" in contract

        rarity_key = False
        for offset in range(max(0, call_offset - 0x300), call_offset - 6):
            if caller[offset : offset + 3] != b"\x48\x8d\x15":
                continue
            target_rva = resolved[CALLER] + offset + 7 + signed32(caller, offset + 3)
            target_offset = image.rva_to_offset(target_rva, 16)
            if target_offset is None:
                continue
            payload, flags, kind = struct.unpack_from("<qII", image.data, target_offset)
            if payload == 27 and flags == 0 and (kind & 0x1F) == 10:
                rarity_key = True
                break
        checks["rarity_key_27_rvalue"] = rarity_key
    else:
        checks["argument_count_is_three"] = False
        checks["argument_array_stack_contract"] = False
        checks["result_slot_contract"] = False
        checks["rarity_key_27_rvalue"] = False

    prologue = announcement[:128]
    checks["announcement_saves_arg_count"] = b"\x45\x8b\xe9" in prologue
    checks["announcement_saves_result"] = b"\x4d\x8b\xf8" in prologue
    checks["announcement_loads_argument_array"] = b"\x4c\x8b\x65\x50" in prologue
    checks["announcement_boolean_result_kind"] = b"\x41\xc7\x47\x0c\x0d\x00\x00\x00" in announcement
    checks["announcement_boolean_true_payload"] = (
        b"\x48\xb8\x00\x00\x00\x00\x00\x00\xf0\x3f" in announcement
    )

    return {
        "ok": all(checks.values()),
        "path": str(path),
        "build": {
            "size": len(data),
            "sha256": sha256,
            "timestamp": f"0x{image.timestamp:08x}",
            "machine": f"0x{image.machine:04x}",
        },
        "resolved_by_name": {name: True for name in resolved},
        "checks": checks,
    }


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify_profile.py <Hero_Siege.exe>", file=sys.stderr)
        return 2
    try:
        result = audit(Path(sys.argv[1]).resolve())
    except (OSError, ValueError, struct.error) as error:
        result = {"ok": False, "error": str(error)}
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result.get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
