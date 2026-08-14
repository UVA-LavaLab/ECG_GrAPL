#!/usr/bin/env python3
"""Assemble and categorize the canonical ReuseBind/ReuseBind-Indexed RV64 sequences."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_SOURCE = PROJECT_ROOT / "bench/src_gem5/reuse_plan_isa_decomposition.S"
FUNCTIONS = (
    "baseline_u32_d32",
    "reuse_plan_m_u32_d32",
    "reuse_plan_i_u32_d32",
)


def assemble(
        source: Path, output: Path,
        compiler: str = "riscv64-linux-gnu-gcc") -> None:
    subprocess.run(
        [
            compiler,
            "-c",
            "-march=rv64gc",
            "-mabi=lp64d",
            str(source),
            "-o",
            str(output),
        ],
        check=True,
        cwd=PROJECT_ROOT,
    )


def disassemble(
        object_path: Path,
        objdump: str = "riscv64-linux-gnu-objdump") -> str:
    return subprocess.run(
        [objdump, "-d", str(object_path)],
        check=True,
        capture_output=True,
        text=True,
        cwd=PROJECT_ROOT,
    ).stdout


def parse_functions(text: str) -> dict[str, list[dict[str, Any]]]:
    functions: dict[str, list[dict[str, Any]]] = {}
    current: str | None = None
    header_re = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
    inst_re = re.compile(
        r"^\s*[0-9a-f]+:\s+([0-9a-f]{4,8})\s+([.\w]+)(?:\s+(.*))?$")
    for raw_line in text.splitlines():
        line = raw_line.strip()
        header = header_re.match(line)
        if header:
            current = header.group(1)
            functions[current] = []
            continue
        if current is None:
            continue
        instruction = inst_re.match(raw_line)
        if not instruction:
            continue
        functions[current].append({
            "raw": instruction.group(1).lower(),
            "mnemonic": instruction.group(2),
            "operands": (instruction.group(3) or "").strip(),
        })
    return functions


def _word(raw: str) -> int:
    # objdump displays the complete instruction word in target byte order.
    return int(raw, 16)


def _custom_mode(instruction: dict[str, Any]) -> tuple[int, int] | None:
    raw = instruction["raw"]
    if len(raw) != 8:
        return None
    word = _word(raw)
    if word & 0x7F != 0x0B:
        return None
    if (word >> 12) & 0x7 != 0x2:
        return None
    funct7 = (word >> 25) & 0x7F
    return funct7 >> 2, funct7 & 0x3


def categorize(instructions: list[dict[str, Any]]) -> dict[str, int]:
    body = [
        instruction for instruction in instructions
        if instruction["mnemonic"] not in ("ret", "jr")
    ]
    categories = {
        "body_instructions": len(body),
        "record_load": 0,
        "destination_extract": 0,
        "address_generation": 0,
        "ordinary_property_load": 0,
        "reuse_bind_load": 0,
        "reuse_bind_iload": 0,
    }
    shifts_seen = 0
    for instruction in body:
        mnemonic = instruction["mnemonic"]
        mode = _custom_mode(instruction)
        if mode == (0x06, 0):
            categories["reuse_bind_load"] += 1
            continue
        if mode == (0x03, 0):
            categories["reuse_bind_iload"] += 1
            continue
        if mnemonic == "ld":
            categories["record_load"] += 1
        elif mnemonic in ("slli", "srli"):
            if shifts_seen < 2:
                categories["destination_extract"] += 1
            else:
                categories["address_generation"] += 1
            shifts_seen += 1
        elif mnemonic == "add":
            categories["address_generation"] += 1
        elif mnemonic == "lw":
            categories["ordinary_property_load"] += 1
        else:
            raise ValueError(
                f"unclassified instruction: {mnemonic} "
                f"{instruction['operands']}")
    return categories


def analyze(text: str) -> dict[str, Any]:
    parsed = parse_functions(text)
    missing = [name for name in FUNCTIONS if name not in parsed]
    if missing:
        raise ValueError(f"missing functions: {', '.join(missing)}")
    rows = {
        name: categorize(parsed[name])
        for name in FUNCTIONS
    }
    rows["reuse_plan_m_u32_d32"]["custom_instruction_word"] = next(
        instruction["raw"] for instruction in parsed["reuse_plan_m_u32_d32"]
        if _custom_mode(instruction) == (0x06, 0))
    rows["reuse_plan_i_u32_d32"]["custom_instruction_word"] = next(
        instruction["raw"] for instruction in parsed["reuse_plan_i_u32_d32"]
        if _custom_mode(instruction) == (0x03, 0))
    baseline = rows["baseline_u32_d32"]["body_instructions"]
    rows["reuse_plan_m_u32_d32"]["instructions_vs_baseline"] = (
        rows["reuse_plan_m_u32_d32"]["body_instructions"] - baseline)
    rows["reuse_plan_i_u32_d32"]["instructions_vs_baseline"] = (
        rows["reuse_plan_i_u32_d32"]["body_instructions"] - baseline)
    return {
        "isa": "RV64GC",
        "record_layout": "dest32|tier2|epoch1_15|epoch2_15",
        "element_bytes": 4,
        "rows": rows,
        "claim_boundary": {
            "reuse_plan_m": (
                "replaces the ordinary property load one-for-one; "
                "destination extraction and address generation remain"),
            "reuse_plan_i": (
                "optional indexed fusion removes destination extraction "
                "and address generation in this canonical sequence"),
            "speedup": "static instruction decomposition only; no timing claim",
        },
    }


def markdown(result: dict[str, Any]) -> str:
    lines = [
        "| Sequence | Body inst. | Record load | Dest extract | Addr gen | "
        "Ordinary load | ReuseBind | ReuseBind-Indexed | Delta vs baseline |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    labels = {
        "baseline_u32_d32": "Baseline",
        "reuse_plan_m_u32_d32": "ReuseBind",
        "reuse_plan_i_u32_d32": "ReuseBind-Indexed",
    }
    for name in FUNCTIONS:
        row = result["rows"][name]
        delta = row.get("instructions_vs_baseline", 0)
        lines.append(
            f"| {labels[name]} | {row['body_instructions']} | "
            f"{row['record_load']} | {row['destination_extract']} | "
            f"{row['address_generation']} | "
            f"{row['ordinary_property_load']} | {row['reuse_bind_load']} | "
            f"{row['reuse_bind_iload']} | {delta:+d} |")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Categorize canonical ReuseBind/ReuseBind-Indexed instruction sequences.")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--compiler", default="riscv64-linux-gnu-gcc")
    parser.add_argument("--objdump", default="riscv64-linux-gnu-objdump")
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with tempfile.TemporaryDirectory(prefix="reuse_plan_isa_") as temp:
        object_path = Path(temp) / "reuse_plan_isa.o"
        assemble(args.source, object_path, args.compiler)
        result = analyze(disassemble(object_path, args.objdump))
    print(
        json.dumps(result, indent=2, sort_keys=True)
        if args.json else markdown(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
